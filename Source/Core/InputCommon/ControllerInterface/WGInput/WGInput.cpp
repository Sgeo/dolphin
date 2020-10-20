// Copyright 2020 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "InputCommon/ControllerInterface/WGInput/WGInput.h"

#include <array>
#include <string_view>

#include <fmt/format.h>

#pragma warning(push)
// Disable warning for virtual functions with no virtual destructor.
#pragma warning(disable : 4265)
// Disable warning for macro replacement list.
#pragma warning(disable : 5104)
#include <wrl.h>
#pragma warning(pop)
#include <roapi.h>
#include <windows.gaming.input.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

#pragma comment(lib, "runtimeobject.lib")

namespace WGI = ABI::Windows::Gaming::Input;
using ABI::Windows::Foundation::Collections::IVectorView;
using Microsoft::WRL::ComPtr;

namespace
{
bool g_runtime_initialized = false;
bool g_runtime_needs_deinit = false;
}  // namespace

namespace ciface::WGInput
{
static constexpr std::string_view SOURCE_NAME = "WGInput";

// These names correspond to the values of the GameControllerButtonLabel enum.
// "None" is not used.
// There are some overlapping names assuming no device exposes both
// GameControllerButtonLabel_XboxLeftBumper and GameControllerButtonLabel_LeftBumper.
// If needed we can prepend "Xbox" to relevant input names on conflict in the future.
static constexpr std::array wgi_button_names = {
    "None",      "Back",     "Start",      "Menu",     "View",     "Pad N",
    "Pad S",     "Pad W",    "Pad E",      "Button A", "Button B", "Button X",
    "Button Y",  "Bumper L", "Trigger L",  "Thumb L",  "Bumper R", "Trigger R",
    "Thumb R",   "Paddle 1", "Paddle 2",   "Paddle 3", "Paddle 4", "Mode",
    "Select",    "Menu",     "View",       "Back",     "Start",    "Options",
    "Share",     "Pad N",    "Pad S",      "Pad W",    "Pad E",    "Letter A",
    "Letter B",  "Letter C", "Letter L",   "Letter R", "Letter X", "Letter Y",
    "Letter Z",  "Cross",    "Circle",     "Square",   "Triangle", "Bumper L",
    "Trigger L", "Thumb L",  "Left 1",     "Left 2",   "Left 3",   "Bumper R",
    "Trigger R", "Thumb R",  "Right 1",    "Right 2",  "Right 3",  "Paddle 1",
    "Paddle 2",  "Paddle 3", "Paddle 4",   "Plus",     "Minus",    "Down Left Arrow",
    "Dial L",    "Dial R",   "Suspension",
};

static constexpr std::array gamepad_trigger_names = {"Trigger L", "Trigger R"};
static constexpr std::array gamepad_axis_names = {"Left X", "Left Y", "Right X", "Right Y"};
static constexpr std::array gamepad_motor_names = {"Motor L", "Motor R", "Trigger L", "Trigger R"};

class Device : public Core::Device
{
public:
  Device(std::string name, Microsoft::WRL::ComPtr<WGI::IRawGameController> raw_controller,
         WGI::IGamepad* gamepad)
      : m_name(std::move(name)), m_raw_controller(raw_controller), m_gamepad(gamepad)
  {
    // Buttons:
    PopulateButtons();

    // Add inputs for IGamepad interface if available.
    if (m_gamepad)
    {
      // Axes:
      const double* axis = &m_gamepad_reading.LeftThumbstickX;
      for (auto& axis_name : gamepad_axis_names)
      {
        AddInput(new NamedAxis(axis, 0.0, -1.0, axis_name));
        AddInput(new NamedAxis(axis, 0.0, +1.0, axis_name));
        ++axis;
      }

      // Triggers:
      const double* trigger = &m_gamepad_reading.LeftTrigger;
      for (auto& trigger_name : gamepad_trigger_names)
      {
        AddInput(new NamedTrigger(trigger, trigger_name));
        ++trigger;
      }

      // Motors:
      double* motor = &m_state_out.LeftMotor;
      for (auto& motor_name : gamepad_motor_names)
      {
        AddOutput(new NamedMotor(motor, motor_name, this));
        ++motor;
      }
    }

    // Add IRawGameController's axes if IGamepad is not available.
    // This may need to change if some devices expose additional axes.
    // Maybe we can determine which additional axes are not in IGamepad's collection.
    const bool use_raw_controller_axes = !m_gamepad;

    if (use_raw_controller_axes)
    {
      // Axes:
      INT32 axis_count = 0;
      if (SUCCEEDED(m_raw_controller->get_AxisCount(&axis_count)))
        m_axes.resize(axis_count);

      u32 i = 0;
      for (auto& axis : m_axes)
      {
        // AddAnalogInputs adds additional "FullAnalogSurface" Inputs.
        AddAnalogInputs(new IndexedAxis(&axis, 0.5, +0.5, i), new IndexedAxis(&axis, 0.5, -0.5, i));
        ++i;
      }
    }

    // Apparently some devices (e.g. DS4) provide the IGameController interface
    // but expose the dpad only on a switch (IRawGameController interface).
    // We'll need to add switches regardless of available interfaces.
    constexpr bool use_raw_controller_switches = true;

    // Switches (Hats):
    if (use_raw_controller_switches)
    {
      INT32 switch_count = 0;
      if (SUCCEEDED(m_raw_controller->get_SwitchCount(&switch_count)))
        m_switches.resize(switch_count);

      u32 i = 0;
      for (auto& swtch : m_switches)
      {
        using gcsp = WGI::GameControllerSwitchPosition;

        WGI::GameControllerSwitchKind switch_kind;
        m_raw_controller->GetSwitchKind(i, &switch_kind);

        AddInput(new IndexedSwitch(&swtch, i, gcsp::GameControllerSwitchPosition_Up));
        AddInput(new IndexedSwitch(&swtch, i, gcsp::GameControllerSwitchPosition_Down));

        if (switch_kind != WGI::GameControllerSwitchKind_TwoWay)
        {
          // If it's not a "two-way" switch (up/down only) then add the left/right inputs.
          AddInput(new IndexedSwitch(&swtch, i, gcsp::GameControllerSwitchPosition_Left));
          AddInput(new IndexedSwitch(&swtch, i, gcsp::GameControllerSwitchPosition_Right));
        }

        ++i;
      }
    }

    // Haptics:
    PopulateHaptics();

    // Battery:
    if (SUCCEEDED(m_raw_controller->QueryInterface(&m_controller_battery)) && m_controller_battery)
    {
      // It seems many controllers provide IGameControllerBatteryInfo with no battery info.
      if (UpdateBatteryLevel())
        AddInput(new Battery(&m_battery_level));
      else
        m_controller_battery = nullptr;
    }
  }

private:
  // `boolean` comes from Windows API. (typedef of unsigned char)
  using ButtonValueType = boolean;

  class Button : public Input
  {
  public:
    explicit Button(const ButtonValueType* button) : m_button(*button) {}
    ControlState GetState() const override { return ControlState(m_button != 0); }

  private:
    const ButtonValueType& m_button;
  };

  // A button with one of the "labels" that WGI provides.
  class NamedButton final : public Button
  {
  public:
    NamedButton(const ButtonValueType* button, std::string_view name) : Button(button), m_name(name)
    {
    }
    std::string GetName() const override { return std::string(m_name); }

  private:
    const std::string_view m_name;
  };

  // A button with no label so we name it by its index.
  class IndexedButton final : public Button
  {
  public:
    IndexedButton(const ButtonValueType* button, u32 index) : Button(button), m_index(index) {}
    std::string GetName() const override { return fmt::format("Button {}", m_index); }

  private:
    u32 m_index;
  };

  class Axis : public Input
  {
  public:
    Axis(const double* axis, double base, double range)
        : m_base(base), m_range(range), m_axis(*axis)
    {
    }

    ControlState GetState() const override { return ControlState(m_axis - m_base) / m_range; }

  protected:
    const double m_base;
    const double m_range;

  private:
    const double& m_axis;
  };

  class NamedAxis final : public Axis
  {
  public:
    NamedAxis(const double* axis, double base, double range, std::string_view name)
        : Axis(axis, base, range), m_name(name)
    {
    }
    std::string GetName() const override
    {
      return fmt::format("{}{}", m_name, m_range < 0 ? '-' : '+');
    }

  private:
    const std::string_view m_name;
  };

  class NamedTrigger final : public Axis
  {
  public:
    NamedTrigger(const double* axis, std::string_view name) : Axis(axis, 0.0, 1.0), m_name(name) {}
    std::string GetName() const override { return std::string(m_name); }

  private:
    const std::string_view m_name;
  };

  class NamedMotor final : public Output
  {
  public:
    NamedMotor(double* motor, std::string_view name, Device* parent)
        : m_motor(*motor), m_name(name), m_parent(*parent)
    {
    }
    std::string GetName() const override { return std::string(m_name); }
    void SetState(ControlState state) override
    {
      if (m_motor == state)
        return;

      m_motor = state;
      m_parent.UpdateMotors();
    }

  private:
    double& m_motor;
    const std::string_view m_name;
    Device& m_parent;
  };

  class IndexedAxis final : public Axis
  {
  public:
    IndexedAxis(const double* axis, double base, double range, u32 index)
        : Axis(axis, base, range), m_index(index)
    {
    }
    std::string GetName() const override
    {
      return fmt::format("Axis {}{}", m_index, m_range < 0 ? '-' : '+');
    }

  private:
    const u32 m_index;
  };

  class IndexedSwitch final : public Input
  {
  public:
    IndexedSwitch(const WGI::GameControllerSwitchPosition* swtch, u32 index,
                  WGI::GameControllerSwitchPosition direction)
        : m_switch(*swtch), m_index(index), m_direction(direction)
    {
    }
    std::string GetName() const override
    {
      return fmt::format("Switch {} {}", m_index, "NESW"[m_direction / 2]);
    }
    ControlState GetState() const override
    {
      if (m_switch == WGI::GameControllerSwitchPosition_Center)
        return 0.0;

      // All of the "inbetween" states (e.g. Up-Right) are one-off from the four cardinal
      // directions. This tests that the current switch state value is within 1 of the desired
      // state.
      const auto direction_diff = std::abs(m_switch - m_direction);
      return ControlState(direction_diff <= 1 || direction_diff == 7);
    }

  private:
    const WGI::GameControllerSwitchPosition& m_switch;
    const u32 m_index;
    const WGI::GameControllerSwitchPosition m_direction;
  };

  class Battery : public Input
  {
  public:
    Battery(const double* level) : m_level(*level) {}

    bool IsDetectable() const override { return false; }

    ControlState GetState() const override { return m_level; }

    std::string GetName() const override { return "Battery"; }

  private:
    const double& m_level;
  };

  class SimpleHaptics : public Output
  {
  public:
    SimpleHaptics(ComPtr<ABI::Windows::Devices::Haptics::ISimpleHapticsController> haptics,
                  ComPtr<ABI::Windows::Devices::Haptics::ISimpleHapticsControllerFeedback> feedback,
                  u32 haptics_index)
        : m_haptics(haptics), m_feedback(feedback), m_haptics_index(haptics_index)
    {
    }

    void SetState(ControlState state) override
    {
      if (m_current_state == state)
        return;

      m_current_state = state;

      if (state)
        m_haptics->SendHapticFeedbackWithIntensity(m_feedback.Get(), state);
      else
        m_haptics->StopFeedback();
    }

  protected:
    u32 GetHapticsIndex() const { return m_haptics_index; }

  private:
    ComPtr<ABI::Windows::Devices::Haptics::ISimpleHapticsController> m_haptics;
    ComPtr<ABI::Windows::Devices::Haptics::ISimpleHapticsControllerFeedback> m_feedback;
    const u32 m_haptics_index;
    ControlState m_current_state = 0;
  };

  class NamedFeedback final : public SimpleHaptics
  {
  public:
    NamedFeedback(ComPtr<ABI::Windows::Devices::Haptics::ISimpleHapticsController> haptics,
                  ComPtr<ABI::Windows::Devices::Haptics::ISimpleHapticsControllerFeedback> feedback,
                  u32 haptics_index, std::string_view feedback_name)
        : SimpleHaptics(haptics, feedback, haptics_index), m_feedback_name(feedback_name)
    {
    }
    std::string GetName() const override
    {
      return fmt::format("{} {}", m_feedback_name, GetHapticsIndex());
    }

  private:
    const std::string_view m_feedback_name;
  };

  void PopulateButtons()
  {
    // Using RawGameController for buttons because it gives us a nice array instead of a bitmask.
    INT32 button_count = 0;
    if (SUCCEEDED(m_raw_controller->get_ButtonCount(&button_count)))
      m_buttons.resize(button_count);

    u32 i = 0;
    for (auto& button : m_buttons)
    {
      WGI::GameControllerButtonLabel lbl = WGI::GameControllerButtonLabel_None;
      m_raw_controller->GetButtonLabel(i, &lbl);

      if (lbl != WGI::GameControllerButtonLabel_None && lbl < wgi_button_names.size())
        AddInput(new NamedButton(&button, wgi_button_names[lbl]));
      else
        AddInput(new IndexedButton(&button, i));

      ++i;
    }
  }

  void PopulateHaptics()
  {
    WGI::IRawGameController2* rgc2 = nullptr;
    if (FAILED(m_raw_controller->QueryInterface(&rgc2)) || !rgc2)
      return;

    IVectorView<ABI::Windows::Devices::Haptics::SimpleHapticsController*>* haptics = nullptr;
    if (FAILED(rgc2->get_SimpleHapticsControllers(&haptics)) || !haptics)
      return;

    unsigned int haptic_count = 0;
    if (FAILED(haptics->get_Size(&haptic_count)))
      return;

    for (unsigned int h = 0; h != haptic_count; ++h)
    {
      ComPtr<ABI::Windows::Devices::Haptics::ISimpleHapticsController> haptic;
      if (FAILED(haptics->GetAt(h, &haptic)))
        continue;

      IVectorView<ABI::Windows::Devices::Haptics::SimpleHapticsControllerFeedback*>* feedbacks =
          nullptr;
      if (FAILED(haptic->get_SupportedFeedback(&feedbacks)) || !feedbacks)
        continue;

      unsigned int feedback_count = 0;
      if (FAILED(haptics->get_Size(&feedback_count)))
        continue;

      for (unsigned int f = 0; f != feedback_count; ++f)
      {
        ComPtr<ABI::Windows::Devices::Haptics::ISimpleHapticsControllerFeedback> feedback;
        if (FAILED(feedbacks->GetAt(f, &feedback)))
          continue;

        UINT16 waveform = 0;
        if (FAILED(feedback->get_Waveform(&waveform)))
          continue;

        std::string_view waveform_name{};

        // Haptic Usage Page from HID spec.
        switch (waveform)
        {
        case 0x1003:
          waveform_name = "Click";
          break;
        case 0x1004:
          waveform_name = "Buzz";
          break;
        case 0x1005:
          waveform_name = "Rumble";
          break;
        }

        if (!waveform_name.data())
        {
          WARN_LOG(PAD, "WGInput: Unknown haptics feedback waveform: %d.", waveform);
          continue;
        }

        AddOutput(new NamedFeedback(haptic, feedback, h, waveform_name));
      }
    }
  }

  std::string GetName() const override { return m_name; }

  std::string GetSource() const override { return std::string(SOURCE_NAME); }

  void UpdateInput() override
  {
    // IRawGameController:
    const auto button_count = UINT32(m_buttons.size());
    const auto switch_count = UINT32(m_switches.size());
    const auto axis_count = UINT32(m_axes.size());
    UINT64 timestamp = 0;
    if (FAILED(m_raw_controller->GetCurrentReading(button_count, m_buttons.data(), switch_count,
                                                   m_switches.data(), axis_count, m_axes.data(),
                                                   &timestamp)))
    {
      ERROR_LOG(PAD, "WGInput: IRawGameController::GetCurrentReading failed.");
    }

    // IGamepad:
    if (m_gamepad && FAILED(m_gamepad->GetCurrentReading(&m_gamepad_reading)))
    {
      ERROR_LOG(PAD, "WGInput: IGamepad::GetCurrentReading failed.");
    }

    // IGameControllerBatteryInfo:
    if (m_controller_battery && !UpdateBatteryLevel())
    {
      DEBUG_LOG(PAD, "WGInput: UpdateBatteryLevel failed.");
    }
  }

  void UpdateMotors() { m_gamepad->put_Vibration(m_state_out); }

  bool UpdateBatteryLevel()
  {
    ABI::Windows::Devices::Power::IBatteryReport* report = nullptr;

    if (FAILED(m_controller_battery->TryGetBatteryReport(&report)) || !report)
      return false;

    using ABI::Windows::System::Power::BatteryStatus;
    BatteryStatus status;
    if (FAILED(report->get_Status(&status)))
      return false;

    switch (status)
    {
    case BatteryStatus::BatteryStatus_NotPresent:
      m_battery_level = 0;
      return true;

    case BatteryStatus::BatteryStatus_Idle:
    case BatteryStatus::BatteryStatus_Charging:
      m_battery_level = BATTERY_INPUT_MAX_VALUE;
      return true;

    default:
      break;
    }

    ABI::Windows::Foundation::IReference<int>*i_remaining = nullptr, *i_full = nullptr;
    int remaining_value = 0;
    int full_value = 0;

    if (report && SUCCEEDED(report->get_RemainingCapacityInMilliwattHours(&i_remaining)) &&
        i_remaining && SUCCEEDED(i_remaining->get_Value(&remaining_value)) &&
        SUCCEEDED(report->get_FullChargeCapacityInMilliwattHours(&i_full)) && i_full &&
        SUCCEEDED(i_full->get_Value(&full_value)))
    {
      m_battery_level = BATTERY_INPUT_MAX_VALUE * remaining_value / full_value;
      return true;
    }

    return false;
  }

  const std::string m_name;

  ComPtr<WGI::IRawGameController> const m_raw_controller;
  std::vector<ButtonValueType> m_buttons;
  std::vector<WGI::GameControllerSwitchPosition> m_switches;
  std::vector<double> m_axes;

  WGI::IGamepad* m_gamepad = nullptr;
  WGI::GamepadReading m_gamepad_reading{};
  WGI::GamepadVibration m_state_out{};

  WGI::IGameControllerBatteryInfo* m_controller_battery = nullptr;
  ControlState m_battery_level = 0;
};

void Init()
{
  if (g_runtime_initialized)
    return;

  const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
  switch (hr)
  {
  case S_OK:
    g_runtime_initialized = true;
    g_runtime_needs_deinit = true;
    break;

  case S_FALSE:
  case RPC_E_CHANGED_MODE:
    g_runtime_initialized = true;
    break;

  default:
    ERROR_LOG(PAD, "WGInput: RoInitialize failed.");
    break;
  }
}

void DeInit()
{
  if (!g_runtime_initialized)
    return;

  if (g_runtime_needs_deinit)
  {
    RoUninitialize();
    g_runtime_needs_deinit = false;
  }

  g_runtime_initialized = false;
}

void PopulateDevices()
{
  if (!g_runtime_initialized)
    return;

  g_controller_interface.RemoveDevice(
      [](const auto* dev) { return dev->GetSource() == SOURCE_NAME; });

  using Microsoft::WRL::Wrappers::HStringReference;

  // WGI Interfaces to potentially use:
  // Gamepad: Buttons, 2x Sticks and 2x Triggers, 4x Vibration Motors
  // RawGameController: Buttons, Switches (Hats), Axes, Haptics
  // The following are not implemented:
  // ArcadeStick: Buttons (probably no need to specialize, literally just buttons)
  // FlightStick: Buttons, HatSwitch, Pitch, Roll, Throttle, Yaw
  // RacingWheel: Buttons, Clutch, Handbrake, PatternShifterGear, Throttle, Wheel, WheelMotor
  // UINavigationController: Directions, Scrolling, etc.

  ComPtr<WGI::IRawGameControllerStatics> raw_stats;
  if (FAILED(
          RoGetActivationFactory(HStringReference(L"Windows.Gaming.Input.RawGameController").Get(),
                                 __uuidof(WGI::IRawGameControllerStatics), &raw_stats)))
  {
    ERROR_LOG(PAD, "WGInput: Failed to get IRawGameControllerStatics.");
    return;
  }

  ComPtr<WGI::IGamepadStatics2> gamepad_stats;
  if (FAILED(RoGetActivationFactory(HStringReference(L"Windows.Gaming.Input.Gamepad").Get(),
                                    __uuidof(WGI::IGamepadStatics2), &gamepad_stats)))
  {
    ERROR_LOG(PAD, "WGInput: Failed to get IGamepadStatics2.");
    return;
  }

  IVectorView<WGI::RawGameController*>* raw_controllers;
  if (FAILED(raw_stats->get_RawGameControllers(&raw_controllers)))
  {
    ERROR_LOG(PAD, "WGInput: get_RawGameControllers failed.");
    return;
  }

  unsigned int raw_count = 0;
  raw_controllers->get_Size(&raw_count);
  for (unsigned i = 0; i != raw_count; ++i)
  {
    ComPtr<WGI::IRawGameController> raw_controller;
    if (SUCCEEDED(raw_controllers->GetAt(i, &raw_controller)) && raw_controller)
    {
      std::string device_name;

      // Attempt to get the controller's name.
      WGI::IRawGameController2* rgc2 = nullptr;
      if (SUCCEEDED(raw_controller->QueryInterface(&rgc2)) && rgc2)
      {
        HSTRING hstr = {};
        if (SUCCEEDED(rgc2->get_DisplayName(&hstr)) && hstr)
          device_name = StripSpaces(WStringToUTF8(WindowsGetStringRawBuffer(hstr, nullptr)));
      }

      if (device_name.empty())
      {
        ERROR_LOG(PAD, "WGInput: Failed to get device name.");
        // Set a default name if we couldn't query the name or it was empty.
        device_name = "Device";
      }

      WGI::IGameController* gamecontroller = nullptr;
      WGI::IGamepad* gamepad = nullptr;
      if (SUCCEEDED(raw_controller->QueryInterface(&gamecontroller)) && gamecontroller)
        gamepad_stats->FromGameController(gamecontroller, &gamepad);

      // Note that gamepad may be nullptr here. The Device class will deal with this.
      auto dev = std::make_shared<Device>(std::move(device_name), raw_controller, gamepad);

      // Only add if it has some inputs/outputs.
      if (dev->Inputs().size() || dev->Outputs().size())
        g_controller_interface.AddDevice(std::move(dev));
    }
  }
}

}  // namespace ciface::WGInput
