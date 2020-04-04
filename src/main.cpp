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
#include <getopt.h>
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
  const char *original_dir = nullptr,
             *destination_dir = nullptr;
};
void print_help(const char *progname);
void mmap_copy(int original, int newfile, const struct stat &stat, const options &opts);
#ifdef HAS_SENDFILE
void sendfile_copy(int original, int newfile, const struct stat &stat, const options &opts);
#endif
options parse_arguments(int argc, char **argv);
void validate_options(const options &);
int main(int argc, char **argv) {
  auto options = parse_arguments(argc, argv);
  validate_options(options);
  if (!fs::is_directory(options.original_dir)) {
    std::cout << options.original_dir << " is not a directory" << std::endl;
    return 1;
  }

  fs::path other = options.destination_dir,
           src = options.original_dir;
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
      //File should exist by this point,
      //but sendfile and mmap don't like size 0 files
      if (stat.st_size > 0) {
#ifdef HAS_SENDFILE
        sendfile_copy(original, newfile, stat, options);
#else
        mmap_copy(original, newfile, stat, options);
#endif
      }
      close(original);
      close(newfile);
      if (options.copy_permissions)
        fs::permissions(newpath, original_perms);
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
  if (new_data == MAP_FAILED)
    handle_error("mmap NEW");
  memcpy(new_data, original_data, stat.st_size);
  munmap(new_data, stat.st_size);
  munmap(original_data, stat.st_size);
}
#ifdef HAS_SENDFILE
void sendfile_copy(int original, int newfile, const struct stat &stat, const options &options) {
  off_t copied = 0;
  ftruncate(newfile, stat.st_size);
  while (copied < stat.st_size) {
    if (-1 == sendfile(newfile, original, &copied, std::min(options.chunk_size, stat.st_size - copied))) {
      handle_error("SENDFILE");
    }
    if (options.print_info)
      std::cout << "Sent " << copied / std::min(0x7ffff000, options.chunk_size) << " chunks" << '\r' << std::flush;
  }
  if (options.print_info)
    std::cout << std::endl;
}
#endif
options parse_arguments(int argc, char **argv) {
  options result;
  if (argc == 1)
    print_help(argv[0]);
  static struct option long_options[] = {
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
      auto q = strtol(optarg, NULL, 10);
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
  } else if (fs::is_directory(opts.original_dir)) {
    std::cerr << "Origin directory '" << opts.original_dir << "' does not exist" << std::endl;
    invalid = true;
  }
  if (opts.destination_dir == nullptr) {
    std::cerr << "Destination directory not specified" << std::endl;
    invalid = true;
  } else if (fs::is_directory(opts.destination_dir)) {
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
            << "\t--chunk-size <size>\t Set the amount copied at once defaults to " << std::numeric_limits<ssize_t>::max() << "\n"
            << "\t--preserve-permissions\tCopy the original permissions from each file and directory\n"
            << std::endl;
  exit(0);
}