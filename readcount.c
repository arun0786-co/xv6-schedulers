#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int initial_count, final_count;
  int fd;
  char buffer[100];
  int bytes_read;

  // Get initial read count
  initial_count = getreadcount();
  printf("Initial read count: %d bytes\n", initial_count);

  // Open a file for reading
  fd = open("README", 0);
  if(fd < 0) {
    printf("Failed to open README file\n");
    exit(1);
  }

  // Read 100 bytes from the file
  bytes_read = read(fd, buffer, 100);
  printf("Read %d bytes from README\n", bytes_read);

  // Close the file
  close(fd);

  // Get final read count
  final_count = getreadcount();
  printf("Final read count: %d bytes\n", final_count);

  // Verify the increase
  if(final_count == initial_count + bytes_read) {
    printf("SUCCESS: Read count increased by %d bytes as expected\n", bytes_read);
  } else {
    printf("ERROR: Expected increase of %d bytes, but got %d bytes\n", 
           bytes_read, final_count - initial_count);
  }

  exit(0);
}
