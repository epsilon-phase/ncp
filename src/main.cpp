#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sys/mman.h>
#ifdef HAS_SENDFILE
#include <sys/sendfile.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#define handle_error(msg) \
  do {                    \
    perror(msg);          \
    exit(EXIT_FAILURE);   \
  } while (0)
namespace fs = std::filesystem;
void mmap_copy(int original, int newfile, const struct stat &stat);
#ifdef HAS_SENDFILE
void sendfile_copy(int original, int newfile, const struct stat &stat);
#endif
int main(int argc, char **argv) {
  if (argc != 3) {
    std::cout << argv[0] << " [source directory] [target directory]" << std::endl;
    return 1;
  }
  if (!fs::is_directory(argv[1])) {
    std::cout << argv[1] << " is not a directory" << std::endl;
    return 1;
  }
  std::vector<fs::directory_entry> directories;
  directories.push_back(fs::directory_entry(argv[1]));

  fs::path other = argv[2],
           src = argv[1];
  long pages = sysconf(_SC_PHYS_PAGES);
  long page_size = sysconf(_SC_PAGE_SIZE);
  long memory_available = (pages * page_size) / 20;
  for (auto i : fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied)) {

    fs::path newpath = other;
    fs::path::iterator a = src.begin(), b = i.path().begin();
    auto original_perms = fs::status(i.path()).permissions();
    // std::cout << i.path() << std::endl;
    while (a != src.end() && *a != "" /*&& b != --i.path().end()*/) {
      a++;
      b++;
    }
    while (b != i.path().end()) {
      newpath /= *b;
      b++;
    }
    if (i.is_directory()) {
      fs::create_directory(newpath);
      fs::permissions(newpath, original_perms);
    } else if (i.is_regular_file() && !fs::exists(newpath)) {
      std::cout << fs::absolute(i.path()) << "->" << fs::absolute(newpath) << std::endl;
      int original = open(fs::absolute(i.path()).c_str(), O_RDONLY);
      if (original == -1) {
        handle_error("ORIGINAL FILE");
      }
      struct stat stat;
      if (-1 == fstat(original, &stat)) {
        handle_error("FSTAT");
      }
      int newfile = open(fs::absolute(newpath).c_str(), O_CREAT | O_RDWR);
      if (newfile == -1) {
        handle_error("NEWFILE");
      }
      if (stat.st_size > 0) {
#ifndef HAS_SENDFILE
        mmap_copy(original, newfile, stat);
#else
        sendfile_copy(original, newfile, stat);
#endif
      }
      close(original);
      close(newfile);
      fs::permissions(newpath, original_perms);
    }
  }

  return 0;
}
void mmap_copy(int original, int newfile, const struct stat &stat) {
  ftruncate(newfile, stat.st_size);
  char *original_data;
  original_data = (char *)mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, original, 0);
  if (original_data == MAP_FAILED) {
    std::cerr << "Options: size=" << stat.st_size << std::endl;
    handle_error("mmap");
  }
  char *new_data;
  new_data = (char *)mmap(NULL, stat.st_size, PROT_WRITE | PROT_READ, MAP_SHARED, newfile, 0);
  if (new_data == MAP_FAILED)
    handle_error("mmap");
  memcpy(new_data, original_data, stat.st_size);
  munmap(new_data, stat.st_size);
  munmap(original_data, stat.st_size);
}
#ifdef HAS_SENDFILE
void sendfile_copy(int original, int newfile, const struct stat &stat) {
  off_t copied = 0;
  ftruncate(newfile, stat.st_size);
  while (copied < stat.st_size) {
    if (-1 == sendfile(newfile, original, &copied, stat.st_size - copied)) {
      handle_error("SENDFILE");
    }
    std::cout << "Sent " << copied / 0x7ffff000 << " chunks" << '\r' << std::flush;
  }
  std::cout << std::endl;
}
#endif