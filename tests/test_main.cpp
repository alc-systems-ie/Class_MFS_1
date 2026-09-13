#include <cstdio>

void run_protocol_tests();
void run_command_codec_tests();

int main()
{
  run_protocol_tests();
  run_command_codec_tests();
  printf("ALL TESTS PASSED\n");
  return 0;
}
