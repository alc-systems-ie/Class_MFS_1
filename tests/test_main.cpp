#include <cstdio>

void run_protocol_tests();
void run_command_codec_tests();
void run_access_key_tests();

int main()
{
  run_protocol_tests();
  run_command_codec_tests();
  run_access_key_tests();
  printf("ALL TESTS PASSED\n");
  return 0;
}
