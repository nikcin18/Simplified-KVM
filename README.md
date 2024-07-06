# Simplified KVM
This project was assignment for course "Computer Architecture and Organization 2" in spring semester of school year 2023/2024. It represents simple virtual machine system made for Linux with KVM (*Kernel-based Virtual Machine*) that is capable of initializing, launching and managing multiple guest operating systems (in following text just *guest systems* or *guests*) simultaneously.

The code is written in C language, with usage of `kvm.h` library for allowing API calls to Linux's KVM.

Guests are capable of communicating with terminal and accessing files on host machine for reading or writing data.

## About hypervisor
At the start of program hypervisor reads guest configuration parameters, guest image files and names of shared files through command line arguments. If command line arguments are invalid, hypervisor signals error and stops working. Otherwise, hypervisor initializes and launches guests where it handles every interrupt made by any guest. All actions for guests are done through separate threads. **Hypervisor code should not be modified**.

## About guest
Guest source code includes wrapper functions for working with I/O and file system. The "guest behaviour" represents source code between comment lines `/* INSERT CODE BELOW THIS LINE */` and `/* INSERT CODE ABOVE THIS LINE */`. This part of code can be modified by user. **The rest of code (including wrapper functions) should not be modified**. Template guest file is included in the project, where multiple guest files can be made by copying template file and modifying each copy.

## About I/O system
Each guest can communicate with terminal, using provided wrapper functions `getchar` and `putchar`. Inside wrapper functions, guest sends requests for working with terminal to hypervisor through I/O port 0x00E9. Size of data sent through port is one byte.

## About file system
Each guest can access data from files that are stored in host machine. Virtual machine system implements file system that imitates POSIX file descriptor system. The file descriptor represents one opened file with either read or write operation allowed. Guest uses provided wrapper functions `fopen`, `fclose`, `fread` and `fwrite`. Guest sends requests for working with file system to hypervisor through I/O port 0x0278. Size of data sent through port is one byte.

File in file system can be local or shared. The local file is visible only to the guest that created that file for writing during current session. If guest attempts to open non-existent local file for reading, hypervisor will signal error that will be passed to guest as return value.

Shared file is visible to all guests. Multiple guests can open same shared file for reading (each guest will receive unique file descriptor) simultaneously. Should guest try to write data to shared file, new local file will be created with same name as shared file and it will be used instead by that guest instead of shared file from now on until the guest shuts down.

In order for hypervisor to differentiate between local files from different guests with same name, every local file will have suffix `".local?"` appended to its name, where `"?"` represents ID of that guest (i.e. for guest with ID 23 suffix `".local23"` is used). The guest will not be aware of suffix in file name, and as such user should not make guest code be dependent of mentioned sufix.

## Launching the hypervisor and setting the guest configuration parameters
The user launches hypervisor using terminal by executing command `mini_hypervisor` with parameters that specify guest system's settings.

### Parameter 1: guest physical memory size
Guest physical memory size is specified using option `-m` or `--memory` in command followed by parameter value. **This is a mandatory parameter**. There are three possible parameter values:
- value `2` (size of physical memory is 2 megabytes)
- value `4` (size of physical memory is 4 megabytes)
- value `8` (size of physical memory is 8 megabytes)

### Parameter 2: guest virtual memory page size
Guest virtual memory page is specified using option `-p` or `--page` in command followed by parameter value. **This is a mandatory parameter**. There are two possible parameter values:
- value `2` (size of virtual memory page is 2 megabytes)
- value `4` (size of virtual memory page is 4 kilobytes)

### Parameter 3: guest image file
Guest image file represents compiled guest file's source code. Upon guest initialization, image file's content is on copied into memory allocated for guest's physical memory. When the guest system is launched, the compiled source code is executed. Parameter is specified using option `-g` or `--guest` in command followed by relative path to guest image file for each of guest systems.

### Parameter 4: shared files
Shared files are specified using option `-f` or `--file` in command followed by relative path to shared file for each of the shared files.

## Example of launching hypervisor
Following command represents virtual machine system where guest physical memory size is 8MB and virtual memory page size is 4KB. Guests are initialized by image files "guest1.img","guest2.img" and "guest3.img". Shared files are "shared1.txt" and "shared2.cpp".
`mini_hypervisor -m 8 -p 4 -g guest1.img guest2.img guest3.img -f shared1.txt shared2.cpp`

## Generating hypervisor object file and guest image files
Hypervisor object file is generated by executing command:
`gcc -lpthread mini_hypervisor.c -o mini_hypervisor`

Guest image file is generated by executing commands with following format:
`$(CC) -m64 -ffreestanding -fno-pic -c -o guest.o guest.c`
`ld -T guest.ld guest.o -o guest.img`

## Important notes
- Two or more guests can use same image file for initializing their memory data, but each guest has separate memory address space, in order to enable every guest to run independently.

- Each guest uses I/O operations indepentently. Because of that, there is no guarantee which guest's `getchar` and `putchar` operations will be executed first.

- If guest opens a shared file for reading, and then opens same file for writing, file descriptor returned from first file opening will be made invalid. This means that attempting to read data from file or close file using that descriptor will result in error return value.

- Hypervisor stores information only about created local files created during current session. Files that are created in previous sessions as local files will not be visible by hypervisor/guest, furthermore they will be overwritten if new local files with same name are required to be created.

## Update notes
### 1.0 Initial version
This is the version of project made as solution for course assignment. Because of specific assignment requirements, entire source code is stored in files "mini_hypervisor.c" and "guest.c".