#include <cstdio>

void run_protocol_tests();
void run_command_codec_tests();
void run_access_key_tests();
void run_device_clock_tests();
void run_access_control_tests();
void run_led_sequencer_tests();
void run_settings_tests();
void run_credentials_tests();
void run_arm_policy_tests();
void run_arming_sequence_tests();
void run_detection_engine_tests();

int main()
{
  run_protocol_tests();
  run_command_codec_tests();
  run_access_key_tests();
  run_device_clock_tests();
  run_access_control_tests();
  run_led_sequencer_tests();
  run_settings_tests();
  run_credentials_tests();
  run_arm_policy_tests();
  run_arming_sequence_tests();
  run_detection_engine_tests();
  printf("ALL TESTS PASSED\n");
  return 0;
}
