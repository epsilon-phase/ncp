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
#include <chrono>
#include <cmath>
#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define handle_error(msg) \
  do {                    \
    perror(msg);          \
    exit(EXIT_FAILURE);   \
  } while (0)
namespace fs = std::filesystem;

const int speed_samples = 30;
struct options {
  ssize_t chunk_size = 512 * 1024 * 1024;
  bool print_info = true;
  bool copy_permissions = true;
  /**
   * Use ansi escapes to clear the line nicely :3
   **/
  bool ansi_escape = true;
  bool is_copying = false;
  bool calibrate_speed = true;
  bool overwrite = true;
  const char *original_dir = nullptr,
             *destination_dir = nullptr;
  std::chrono::time_point<std::chrono::steady_clock> last_copy;
  ssize_t last_copy_size[speed_samples],
      total_copied = 0;
  double last_copy_speed[speed_samples];
  double update_speed = 1 / 60.0;
  fs::path current_dest;
  void add_sample(ssize_t);
  void add_sample(double);
  ssize_t avg_chunk_size() const;
  double avg_chunk_speed() const;
  void track_copied(ssize_t);
};
static volatile bool is_killed = false;
void handle_termination(int sigterm);
void print_help(const char *progname);
void perf_mark(options &);
void perf_update(options &);
void mmap_copy(int original, int newfile, const struct stat &stat, options &opts);
#ifdef HAS_SENDFILE
void sendfile_copy(int original, int newfile, const struct stat &stat, options &opts);
#endif
void copy_file(int original, int newfile, const struct stat &stat, options &options) {
#ifdef HAS_SENDFILE
  sendfile_copy(original, newfile, stat, options);
#else
  mmap_copy(original, newfile, stat, options);
#endif
}
void print_size_unit(ssize_t);
options parse_arguments(int argc, char **argv);
void validate_options(const options &);
void print_progress(ssize_t copied, ssize_t total, const options &);
void handle_death(int, int, const options &);
void copy_directory(options &options, fs::path &other, fs::path &src);
void copy_single_file(options &options, const fs::path &other, const fs::path &src);
int main(int argc, char **argv) {
  auto options = parse_arguments(argc, argv);
  validate_options(options);
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = &handle_termination;
  if (!isatty(fileno(stdout))) {
    options.print_info = false;
  }
  sigaction(SIGTERM, &action, NULL);
  fs::path other = options.destination_dir,
           src = options.original_dir;
  std::cout << "Currently working on:" << std::endl;
  if (fs::is_directory(src)) {
    copy_directory(options, other, src);
  } else if (fs::is_regular_file(src)) {
    copy_single_file(options, other, src);
  }

  return 0;
}

void copy_directory(options &options, fs::path &other, fs::path &src) {
  for (auto i : fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied)) {
    fs::path newpath = other;
    fs::path::iterator a = src.begin(), b = i.path().begin();
    auto original_perms = fs::status(i.path()).permissions();
    while (a != src.end() && *a != "") {
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
    } else if (i.is_regular_file()) {
      if (fs::exists(newpath) && !options.overwrite) {
        continue;
      }
      copy_single_file(options, newpath, i.path());
    }
  }
}
void copy_single_file(options &options, const fs::path &other, const fs::path &src) {
  auto original_perms = fs::status(src).permissions();
  if (options.print_info)
    std::cout << "\x1b[1A\x1b[1000D\x1b[0K" << fs::absolute(src) << "->" << fs::absolute(other) << std::endl;
  int original = open(fs::absolute(src).c_str(), O_RDONLY);
  if (original == -1) {
    handle_error("ORIGINAL FILE");
  }
  struct stat stat;
  if (-1 == fstat(original, &stat)) {
    handle_error("FSTAT");
  }
  int newfile = open(fs::absolute(other).c_str(), O_CREAT | O_RDWR);
  options.current_dest = other;
  if (newfile == -1) {
    handle_error("NEWFILE");
  }
  // Doing this permits running 'rm' on the resulting file, even if the program aborts prior to
  // finishing the copy
  if (options.copy_permissions)
    fs::permissions(other, original_perms);
  // File should exist by this point,
  // but sendfile and mmap don't like size 0 files
  if (stat.st_size > 0) {
    options.is_copying = true;
    copy_file(original, newfile, stat, options);
    options.is_copying = false;
  }
  handle_death(original, newfile, options);
  close(original);
  close(newfile);
}

void mmap_copy(int original, int newfile, const struct stat &stat, options &options) {
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
    perf_mark(options);
    memcpy(offset_out, offset_in, chunk_size);
    options.add_sample(chunk_size);
    perf_update(options);
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
void sendfile_copy(int original, int newfile, const struct stat &stat, options &options) {
  off_t copied = 0;
  ftruncate(newfile, stat.st_size);
  while (copied < stat.st_size) {
    perf_mark(options);
    if (-1 == sendfile(newfile, original, &copied, std::min(options.chunk_size, stat.st_size - copied))) {
      handle_error("SENDFILE");
    }
    options.add_sample(std::min(options.chunk_size, stat.st_size - copied));
    perf_update(options);
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
      {"calibrate-speed", no_argument, reinterpret_cast<int *>(&result.calibrate_speed), true},
      {"update-speed", required_argument, 0, 'u'},
      {"no-clobber", no_argument, reinterpret_cast<int *>(&result.overwrite), false},
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
    case 'u': {
      char *end = nullptr;
      auto q = strtod(optarg, &end);
      result.update_speed = 1.0 / q;
    } break;
    case 'c': {
      char *end = nullptr;
      auto q = strtol(optarg, &end, 10);
      if (end != nullptr) {
        // fallthrough is good actually :3
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
  for (int i = 0; i < speed_samples; i++) {
    result.last_copy_size[i] = result.chunk_size;
    result.last_copy_speed[i] = 1.0;
  }
  return result;
}
void validate_options(const options &opts) {
  bool invalid = false;
  bool operating_on_directory = false;
  if (opts.original_dir == nullptr) {
    std::cerr << "Origin directory not specified" << std::endl;
    invalid = true;
  } else if (!fs::exists(opts.original_dir)) {
    std::cerr << "Origin directory '" << opts.original_dir << "' does not exist" << std::endl;
    invalid = true;
  }
  operating_on_directory = fs::is_directory(opts.original_dir);
  if (opts.destination_dir == nullptr) {
    std::cerr << "Destination directory not specified" << std::endl;
    invalid = true;
  } else if (operating_on_directory && !fs::is_directory(opts.destination_dir)) {
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
            << "\t--calibrate-speed\tAttempts to maintain a consistent update speed by adjusting chunk sizes.\n"
            << "\t--update-speed <N>\tAttempts to maintain N update prints per second. Only has effects if the --calibrate-speed flag is also specified\n"
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
  std::cout << "\x1b[1000D\x1b[0K" << options.current_dest.filename() << std::setw(8) << std::setprecision(5) << progress << "%";
  if (options.calibrate_speed) {
    std::cout << " cs=";
    print_size_unit(options.chunk_size);
  }
  std::cout << " copied ";
  print_size_unit(options.total_copied);
  std::cout << std::flush;
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
void perf_mark(options &options) {
  options.last_copy = std::chrono::steady_clock::now();
}
void perf_update(options &options) {
  if (!options.calibrate_speed)
    return;
  auto current_time = std::chrono::steady_clock::now();
  std::chrono::duration<double> duration = current_time - options.last_copy;
  options.add_sample(duration.count());
  ssize_t new_size = std::floor(options.avg_chunk_size() / options.avg_chunk_speed()) * options.update_speed;
  // std::cout << "Changing chunk size from " << options.chunk_size << " to " << new_size << std::endl;
  options.chunk_size = std::max(1024l, (ssize_t)std::floor(new_size));
}
void print_size_unit(ssize_t s) {
  std::cout << std::setprecision(4);
  if (s >= 1024 * 1024 * 1024l * 1024) {
    std::cout << s / (1024.0 * 1024 * 1024 * 1024) << "Tb";
  } else if (s >= 1024 * 1024 * 1024) {
    std::cout << s / (1024.0 * 1024 * 1024) << "Gb";
  } else if (s >= 1024 * 1024) {
    std::cout << s / (1024.0 * 1024.0) << "Mb";
  } else {
    std::cout << s / 1024.0 << "Kb";
  }
}
void options::add_sample(ssize_t s) {
  for (int i = speed_samples - 1; i > 0; i--) {
    last_copy_size[i] = last_copy_size[i - 1];
  }
  last_copy_size[0] = s;
  track_copied(s);
}
void options::add_sample(double s) {
  for (int i = speed_samples - 1; i > 0; i--) {
    last_copy_speed[i] = last_copy_speed[i - 1];
  }
  last_copy_speed[0] = s;
}
ssize_t options::avg_chunk_size() const {
  ssize_t s = 0;
  for (int i = 0; i < speed_samples; i++)
    s += last_copy_size[i];
  return s / speed_samples;
}
double options::avg_chunk_speed() const {
  double s = 0;
  for (int i = 0; i < speed_samples; i++)
    s += last_copy_speed[i];
  return s / speed_samples;
}
void options::track_copied(ssize_t s) {
  total_copied += s;
} 