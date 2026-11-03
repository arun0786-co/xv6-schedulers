#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

// Checks getreadcount(): read 100 bytes from a file and see the count grow.
int
main(void)
{
  char buf[100];
  uint before, after, grew;
  int fd, n;

  before = getreadcount();
  printf("read count before: %u\n", before);

  if((fd = open("/README", O_RDONLY)) < 0){
    fprintf(2, "readcount: cannot open /README\n");
    exit(1);
  }
  n = read(fd, buf, sizeof(buf));
  close(fd);
  if(n != sizeof(buf)){
    fprintf(2, "readcount: read returned %d, expected %d\n", n, (int)sizeof(buf));
    exit(1);
  }

  after = getreadcount();
  printf("read count after:  %u\n", after);

  // Unsigned subtraction gives the right difference even when the count
  // wrapped around to 0 in between.
  grew = after - before;
  if(grew < (uint)n){
    printf("FAIL: the count grew by %u after reading %d bytes\n", grew, n);
    exit(1);
  }
  printf("OK: the count grew by %u after reading %d bytes\n", grew, n);
  if(grew > (uint)n)
    printf("(other processes read %u bytes in the meantime)\n", grew - n);
  exit(0);
}
