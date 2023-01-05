#include "types.h"
#include "user.h"

int main() {
  int dummy = 0;
  //4e9 is the original value
  for (unsigned int i = 0; i < 10000; i++) {
    dummy += i;
  }
  exit();
}