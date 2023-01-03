#include "types.h"
#include "user.h"

int main() {
  if (fork() != 0) {
    wait();
  }
  else {
    char *argv[] = {"short", 0};
    exec("short", argv);
  }
  
  exit();
}