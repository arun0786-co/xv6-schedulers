#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

// usage: readrace [procs] [reads]
// Several processes call read() for one byte at a time, all at once, so on a
// machine with several CPUs the kernel updates the read count from several
// CPUs at the same time. The count must grow by exactly procs * reads.
int
main(int argc, char *argv[])
{
  int procs = argc > 1 ? atoi(argv[1]) : 3;
  int reads = argc > 2 ? atoi(argv[2]) : 20000;
  char name[3] = "r0";
  uint before, grew;

  if(procs < 1 || procs > 9 || reads < 1){
    fprintf(2, "usage: readrace [procs 1-9] [reads]\n");
    exit(1);
  }
  before = getreadcount();
  for(int i = 0; i < procs; i++){
    name[1] = '0' + i;
    if(fork() == 0){
      // each child reads its own file, so they do not wait for each
      // other on one inode lock
      char c;
      int fd = open(name, O_CREATE | O_RDWR);
      for(int k = 0; k < 64; k++)
        write(fd, "x", 1);
      close(fd);
      fd = open(name, O_RDONLY);
      for(int done = 0; done < reads; ){
        if(read(fd, &c, 1) == 1){
          done++;
        } else {
          close(fd);
          fd = open(name, O_RDONLY);
        }
      }
      close(fd);
      exit(0);
    }
  }
  for(int i = 0; i < procs; i++)
    wait(0);
  grew = getreadcount() - before;
  for(int i = 0; i < procs; i++){
    name[1] = '0' + i;
    unlink(name);
  }
  printf("readrace: expected %d, counted %u\n", procs * reads, grew);
  exit(grew == (uint)(procs * reads) ? 0 : 1);
}
