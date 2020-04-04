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
#include <cmath>
#include <getopt.h>
#include <signal.h>
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

struct options {
  ssize_t chunk_size = std::numeric_limits<ssize_t>::max();
  bool print_info = true;
  bool copy_permissions = true;
  /**
   * Use ansi escapes to clear the line nicely :3
   * */
  bool ansi_escape = true;
  bool is_copying = false;
  const char *original_dir = nullptr,
             *destination_dir = nullptr;
  fs::path current_dest;
};
static volatile bool is_killed = false;
void handle_termination(int sigterm);
void print_help(const char *progname);
void mmap_copy(int original, int newfile, const struct stat &stat, const options &opts);
#ifdef HAS_SENDFILE
void sendfile_copy(int original, int newfile, const struct stat &stat, const options &opts);
#endif
options parse_arguments(int argc, char **argv);
void validate_options(const options &);
void print_progress(ssize_t copied, ssize_t total, const options &);
void handle_death(int, int, const options &);
int main(int argc, char **argv) {
  auto options = parse_arguments(argc, argv);
  validate_options(options);
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = &handle_termination;
  sigaction(SIGTERM, &action, NULL);
  fs::path other = options.destination_dir,
           src = options.original_dir;
  std::cout << "Currently working on:" << std::endl;
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
      if (options.copy_permissions)
        fs::permissions(newpath, original_perms);
    } else if (i.is_regular_file() && !fs::exists(newpath)) {
      if (options.print_info)
        std::cout << "\x1b[1A\x1b[1000D\x1b[0K" << fs::absolute(i.path()) << "->" << fs::absolute(newpath) << std::endl;
      int original = open(fs::absolute(i.path()).c_str(), O_RDONLY);
      if (original == -1) {
        handle_error("ORIGINAL FILE");
      }
      struct stat stat;
      if (-1 == fstat(original, &stat)) {
        handle_error("FSTAT");
      }
      int newfile = open(fs::absolute(newpath).c_str(), O_CREAT | O_RDWR);
      options.current_dest = newpath;
      if (newfile == -1) {
        handle_error("NEWFILE");
      }
      //Doing this permits running 'rm' on the resulting file, even if the program aborts prior to
      //finishing the copy
      if (options.copy_permissions)
        fs::permissions(newpath, original_perms);
      //File should exist by this point,
      //but sendfile and mmap don't like size 0 files
      if (stat.st_size > 0) {
        options.is_copying = true;
#ifdef HAS_SENDFILE
        sendfile_copy(original, newfile, stat, options);
#else
        mmap_copy(original, newfile, stat, options);
#endif
        options.is_copying = false;
      }
      handle_death(original, newfile, options);
      close(original);
      close(newfile);
    }
  }

  return 0;
}
void mmap_copy(int original, int newfile, const struct stat &stat, const options &options) {
  ftruncate(newfile, stat.st_size);
  char *original_data;
  original_data = (char *)mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, original, 0);
  if (original_data == MAP_FAILED) {
    std::cerr << "Options: size=" << stat.st_size << std::endl;
    handle_error("mmap ORIGINAL");
  }
  char *new_data;
  new_data = (char *)mmap(NULL, stat.st_size, PROT_WRITE | PROT_READ, MAP_SHARED, newfile, 0);
  char *offset_out = new_data,
       *offset_in = original_data;

  if (new_data == MAP_FAILED)
    handle_error("mmap NEW");
  ssize_t copied = 0,
          chunk_size;
  while (copied < stat.st_size) {
    chunk_size = std::min(stat.st_size - copied, options.chunk_size);
    memcpy(offset_out, offset_in, chunk_size);
    copied += chunk_size;
    offset_in += chunk_size;
    offset_out += chunk_size;
    print_progress(copied, stat.st_size, options);
    handle_death(original, newfile, options);
  }
  munmap(new_data, stat.st_size);
  munmap(original_data, stat.st_size);
}
#ifdef HAS_SENDFILE
void sendfile_copy(int original, int newfile, const struct stat &stat, const options &options) {
  off_t copied = 0;
  ftruncate(newfile, stat.st_size);
  int chunks = 0;
  while (copied < stat.st_size) {
    if (-1 == sendfile(newfile, original, &copied, std::min(options.chunk_size, stat.st_size - copied))) {
      handle_error("SENDFILE");
    }
    print_progress(copied, stat.st_size, options);
    handle_death(original, newfile, options);
  }
}
#endif
options parse_arguments(int argc, char **argv) {
  options result;
  if (argc == 1)
    print_help(argv[0]);
  struct option long_options[] = {
      {"help", no_argument, nullptr, 'h'},
      {"preserve-permissions", no_argument, reinterpret_cast<int *>(&result.copy_permissions), true},
      {"disregard-permissions", no_argument, reinterpret_cast<int *>(&result.copy_permissions), false},
      {"silent", no_argument, reinterpret_cast<int *>(&result.print_info), false},
      {"loud", no_argument, reinterpret_cast<int *>(&result.print_info), true},
      {"chunk-size", required_argument, 0, 'c'},
      {0, 0, 0, 0}};
  int option_index;
  int c;
  while (1) {
    c = getopt_long(argc, argv, "hc:", long_options, &option_index);
    if (c == -1)
      break;
    switch (c) {
    case 0:
      if (long_options[option_index].flag != 0)
        break;
      break;
    case 'h':
      print_help(argv[0]);
      break;
    case 'c': {
      char *end = nullptr;
      auto q = strtol(optarg, &end, 10);
      if (end != nullptr) {
        //fallthrough is good actually :3
        switch (*end) {
        case 'g':
        case 'G':
          q *= 1024;
        case 'm':
        case 'M':
          q *= 1024;
        case 'k':
        case 'K':
          q *= 1024;
          break;
        }
      }

      result.chunk_size = q;
    } break;
    }
  }
  if (optind < argc) {
    while (optind < argc) {
      if (result.original_dir == nullptr)
        result.original_dir = argv[optind++];
      else if (result.destination_dir == nullptr)
        result.destination_dir = argv[optind++];
      else
        optind++;
    }
  }
  return result;
}
void validate_options(const options &opts) {
  bool invalid = false;
  if (opts.original_dir == nullptr) {
    std::cerr << "Origin directory not specified" << std::endl;
    invalid = true;
  } else if (!fs::is_directory(opts.original_dir)) {
    std::cerr << "Origin directory '" << opts.original_dir << "' does not exist" << std::endl;
    invalid = true;
  }
  if (opts.destination_dir == nullptr) {
    std::cerr << "Destination directory not specified" << std::endl;
    invalid = true;
  } else if (!fs::is_directory(opts.destination_dir)) {
    std::cerr << "Destination directory '" << opts.destination_dir << "' does not exist" << std::endl;
    invalid = true;
  }
  if (opts.chunk_size < 0) {
    std::cerr << "chunk size cannot be negative" << std::endl;
    invalid = true;
  }
  if (invalid)
    exit(1);
}
void print_help(const char *argv) {
  std::cout << "Syntax: " << argv[0] << " [options] (source directory) (destination directory)\n"
            << "Options:\n"
            << "\t--disregard-permissions\t Do not copy permissions. This is usually not what you want.\n"
            << "\t--silent\t Don't print anything\n"
            << "\t--loud\t On by default. Print things\n"
            << "\t--chunk-size <size>\t Set the amount copied at once defaults to " << std::numeric_limits<ssize_t>::max() << " bytes.\n"
            << "\t\t Add a suffix of k,m, or g to specify the unit,\n"
            << "\t\t since it's not fun to remember the exact size of each of those units\n"
            << "\t--preserve-permissions\tCopy the original permissions from each file and directory\n"
            << std::endl;
  exit(0);
}
void print_progress(ssize_t copied, ssize_t total, const options &options) {
  if (!options.print_info)
    return;
  double progress = copied;
  progress /= total;
  progress *= 100.0;
  std::cout << "\x1b[1000D\x1b[0K" << options.current_dest << std::setw(8) << std::setprecision(5) << progress << "%" << std::flush;
}
void handle_termination(int sigterm) {
  is_killed = true;
}
void handle_death(int original_fd, int new_fd, const options &options) {
  if (!is_killed)
    return;
  if (options.is_copying) {
    close(original_fd);
    close(new_fd);
    fs::remove(options.current_dest);
  }
  exit(1);
}