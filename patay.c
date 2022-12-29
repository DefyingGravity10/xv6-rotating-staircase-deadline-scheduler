#include "types.h"
#include "user.h"

int main() {
  int dummy = 0;
  //4e9 is the original value
  for (unsigned int i = 0; i < 1e9; i++) {
    dummy += i;
  }
  kill(5);
  
  exit();
}