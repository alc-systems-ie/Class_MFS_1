#include <cstdio>

void run_protocol_tests();
void run_command_codec_tests();
void run_access_key_tests();
void run_device_clock_tests();

int main()
{
  run_protocol_tests();
  run_command_codec_tests();
  run_access_key_tests();
  run_device_clock_tests();
  printf("ALL TESTS PASSED\n");
  return 0;
}
