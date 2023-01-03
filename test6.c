#include "types.h"
#include "user.h"

int main() {
  schedlog(100000000);

  for (int i = 0; i < 3; i++) {
    if (fork() == 0) {
      char *argv[] = {"cf", 0};
      exec("cf", argv);
    }
  }

  for (int i = 0; i < 3; i++) {
    wait();
  }

  shutdown();
}