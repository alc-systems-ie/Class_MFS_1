#include <cassert>
#include <cstdio>

#include "settings.hpp"

void run_settings_tests()
{
  using namespace alc;

  Settings settings;
  protocol::Command command;

  // Defaults match the firmware's historical Kconfig values, so a device that
  // has never been configured behaves exactly as it did before this feature.
  assert(settings.Activations() == 1);
  assert(settings.ThresholdLsb() == 302);
  assert(settings.CooldownSeconds() == 0);
  assert(settings.DelaySeconds() == 0);
  assert(settings.OperatingMode() == protocol::Mode::TriggerOnly);

  command.activations     = 3;
  command.cooldownByte    = 128;
  command.sensitivityByte = 255;
  command.delayCode       = 119;
  command.mode            = protocol::Mode::ReportOnly;

  // An ENGINEER's command (slot 1-7) applies everything EXCEPT the mode.
  assert(settings.ApplyFrom(command, false));
  assert(settings.Activations() == 3);
  assert(settings.CooldownSeconds() == 60);
  assert(settings.ThresholdLsb() == 40);
  assert(settings.DelaySeconds() == 3600);
  assert(settings.OperatingMode() == protocol::Mode::TriggerOnly);

  // The same command again from an engineer changes nothing: the differing mode
  // is ignored, not counted as a change, so NVS is not rewritten.
  assert(!settings.ApplyFrom(command, false));

  // The Network Manager's slot-0 command does change the mode.
  assert(settings.ApplyFrom(command, true));
  assert(settings.OperatingMode() == protocol::Mode::ReportOnly);

  // A change to the delay ALONE must still be detected.
  command.delayCode = 127;
  assert(settings.ApplyFrom(command, false));
  assert(settings.DelaySeconds() == 32400);
  assert(!settings.ApplyFrom(command, true));

  printf("settings: OK\n");
}
