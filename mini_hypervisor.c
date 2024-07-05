#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/kvm.h>

#define SIZE_4KB (0x1000)
#define SIZE_2MB (0x200000)
#define SIZE_4MB (0x400000)
#define SIZE_8MB (0x800000)
#define PDE64_PRESENT 1
#define PDE64_RW (1U << 1)
#define PDE64_USER (1U << 2)
#define PDE64_PS (1U << 7)

// CR4
#define CR4_PAE (1U << 5)

// CR0
#define CR0_PE 1u
#define CR0_PG (1U << 31)

#define EFER_LME (1U << 8)
#define EFER_LMA (1U << 10)

#define PORT_IO 0x00E9
#define PORT_FILE 0x0278

#define FILE_OPEN_R 0x1
#define FILE_OPEN_W 0x2
#define FILE_CLOSE 0x3
#define FILE_READ 0x4
#define FILE_WRITE 0x5

#define FSTATE1_NONE 0
#define FSTATE1_OPEN_R 1
#define FSTATE1_OPEN_W 2
#define FSTATE1_CLOSE 3
#define FSTATE1_READ 4
#define FSTATE1_WRITE 5
#define FSTATE2_NONE 6
#define FSTATE2_START 7
#define FSTATE2_FILENAME 8
#define FSTATE2_FD 9
#define FSTATE2_CHAR 10

typedef struct
{
    void *data;
    void *next;
} LLNode;

typedef LLNode LinkedList;

typedef struct
{
    char *name;
    char canRead;  // 0 - no, 1 - yes
    char canWrite; // 0 - no, 1 - yes
    int guestFd;
    int hostFd;
} MyFile;

typedef struct
{
    int id;
    int memorySize;
    int pageSize;
    char *guestFile;
    int kvmFd;
    int sharedFileCount;
    int nextGuestFd;
    LinkedList *sharedFiles;
} GuestSettings;

static int pushString(LinkedList **list, char *s)
{
    char *str = (char *)malloc(strlen(s) + 1);
    if (str == NULL)
    {
        return -1;
    }
    strcpy(str, s);
    LLNode *elem = (LLNode *)malloc(sizeof(LLNode));
    if (elem == NULL)
    {
        free(str);
        return -1;
    }
    elem->data = str;
    elem->next = *list;
    *list = elem;
    return 0;
}

char *localizeFilename(char *filename, int id)
{
    int fl = strlen(filename);
    int l = fl + 1;
    l += 6; // .local
    if (id == 0)
        l += 1;
    else
    {
        int t = id;
        while (t > 0)
        {
            l += 1;
            t /= 10;
        }
    }
    char *result = (char *)malloc(l);
    if (!result)
        return NULL;
    strncpy(result, filename, fl);
    strncpy(result + fl, ".local", 6);
    result[l - 1] = '\0';
    if (id == 0)
        result[l - 2] = '0';
    else
    {
        int t = id, k = l - 2;
        while (t > 0)
        {
            result[k] = (char)('0' + t % 10);
            k--;
            t /= 10;
        }
    }
    return result;
}

char *extendFilename(char *filename, char c)
{
    int l;
    if (!filename)
        l = 2;
    else
        l = strlen(filename) + 2;
    char *result = (char *)malloc(l);
    if (!result)
    {
        return NULL;
    }
    if (filename)
        strcpy(result, filename);
    result[l - 2] = c;
    result[l - 1] = '\0';
    return result;
}

char *copyFilename(char *filename)
{
    if (!filename)
        return NULL;
    int l = strlen(filename);
    char *result = (char *)malloc(l + 1);
    if (!result)
        return NULL;
    strcpy(result, filename);
    return result;
}

static int pushFile(LinkedList **list, MyFile *file)
{
    LLNode *elem = (LLNode *)malloc(sizeof(LLNode));
    if (elem == NULL)
    {
        return -1;
    }
    elem->data = file;
    elem->next = *list;
    *list = elem;
    return 0;
}

static void deleteFile(LinkedList **list, MyFile *file)
{
    if (!(*list))
        return;
    LLNode *temp = NULL;
    if ((*list)->data == file)
    {
        temp = *list;
        *list = (*list)->next;
    }
    else
    {
        LLNode *prev = *list;
        while (prev->next)
        {
            LLNode *nextNode = (LLNode *)prev->next;
            if (nextNode->data == file)
            {
                temp = nextNode;
                prev->next = temp->next;
                break;
            }
            prev = nextNode;
        }
    }
    if (temp)
    {
        MyFile *tempFile = (MyFile *)temp->data;
        if (tempFile->hostFd > -1)
            close(tempFile->hostFd);
        free(tempFile->name);
        free(tempFile);
        free(temp);
    }
}

static void deleteFileList(LinkedList **list)
{
    while (*list)
    {
        deleteFile(list, (*list)->data);
    }
}

static void deleteList(LinkedList *list, int deleteData)
{
    while (list)
    {
        LLNode *temp = list;
        list = list->next;
        if (deleteData)
            free(temp->data);
        free(temp);
    }
}

printFileList(LinkedList *list)
{
    printf("File list:\n");
    LLNode *temp = list;
    while (temp)
    {
        printf("\tNew file\n");
        MyFile *tempFile = (MyFile *)temp->data;
        printf("\t\tName: '%s'\n", tempFile->name);
        printf("\t\tGuest fd: %d\n", tempFile->guestFd);
        printf("\t\tHost fd: %d\n", tempFile->hostFd);
        printf("\t\tCan read: %d\n", tempFile->canRead);
        printf("\t\tCan write: %d\n", tempFile->canWrite);
        temp = temp->next;
    }
}

static int openFile(LinkedList **sharedFileSystem, LinkedList **localFileSystem, char *name, char toRead, GuestSettings *guestSettings)
{
    char *localName = localizeFilename(name, guestSettings->id);
    if (!localName)
        return -1;
    MyFile *localFile = NULL;
    MyFile *sharedFile = NULL;
    LLNode *temp = *sharedFileSystem;
    while (temp)
    {
        MyFile *tempFile = (MyFile *)temp->data;
        if (strcmp(tempFile->name, name) == 0)
        {
            sharedFile = tempFile;
            break;
        }
        temp = temp->next;
    }
    if (!sharedFile)
    {
        temp = *localFileSystem;
        while (temp)
        {
            MyFile *tempFile = (MyFile *)temp->data;
            if (strcmp(tempFile->name, localName) == 0)
            {
                localFile = tempFile;
                break;
            }
            temp = temp->next;
        }
        if (!localFile)
        {
            if (toRead)
                return -1;
            MyFile *newFile = (MyFile *)malloc(sizeof(MyFile));
            if (!newFile)
            {
                free(localName);
                return -1;
            }
            newFile->hostFd = open(localName, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
            if (newFile->hostFd < 0)
            {
                free(localName);
                free(newFile);
                return -1;
            }
            if (pushFile(localFileSystem, newFile) != 0)
            {
                close(newFile->hostFd);
                free(localName);
                free(newFile);
                return -1;
            }
            newFile->canRead = 0;
            newFile->canWrite = 1;
            newFile->name = localName;
            newFile->guestFd = guestSettings->nextGuestFd;
            guestSettings->nextGuestFd += 1;
            // printFileList(*localFileSystem);
            return newFile->guestFd;
        }
        else
        {
            MyFile *newFile = (MyFile *)malloc(sizeof(MyFile));
            if (!newFile)
            {
                free(localName);
                return -1;
            }
            newFile->hostFd = open(localName, toRead ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC), S_IRWXU | S_IRWXG | S_IRWXO);
            if (newFile->hostFd < 0)
            {
                free(localName);
                free(newFile);
                return -1;
            }
            if (pushFile(localFileSystem, newFile) != 0)
            {
                close(newFile->hostFd);
                free(localName);
                free(newFile);
                return -1;
            }
            newFile->canRead = toRead;
            newFile->canWrite = 1 - toRead;
            newFile->name = localName;
            newFile->guestFd = guestSettings->nextGuestFd;
            guestSettings->nextGuestFd += 1;
            // printFileList(*localFileSystem);
            return newFile->guestFd;
        }
    }
    else
    {
        if (sharedFile->canRead)
        {
            if (toRead)
            {
                free(localName);
                MyFile *newFile = (MyFile *)malloc(sizeof(MyFile));
                if (!newFile)
                {
                    return -1;
                }
                newFile->hostFd = open(name, O_RDONLY);
                if (newFile->hostFd < 0)
                {
                    free(newFile);
                    return -1;
                }
                if (pushFile(localFileSystem, newFile) != 0)
                {
                    close(newFile->hostFd);
                    free(newFile);
                    return -1;
                }
                newFile->canRead = 1;
                newFile->canWrite = 0;
                newFile->name = name;
                newFile->guestFd = guestSettings->nextGuestFd;
                guestSettings->nextGuestFd += 1;
                // printFileList(*localFileSystem);
                return newFile->guestFd;
            }
            else
            {
                sharedFile->canRead = 0;
                temp = *localFileSystem;
                while (temp)
                {
                    MyFile *tempFile = (MyFile *)temp->data;
                    if (strcmp(tempFile->name, name) == 0)
                    {
                        close(tempFile->hostFd);
                        tempFile->hostFd = -1;
                        tempFile->guestFd = -1;
                    }
                    temp = temp->next;
                }
                MyFile *newFile = (MyFile *)malloc(sizeof(MyFile));
                if (!newFile)
                {
                    free(localName);
                    return -1;
                }
                newFile->hostFd = open(localName, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
                if (newFile->hostFd < 0)
                {
                    free(localName);
                    free(newFile);
                    return -1;
                }
                if (pushFile(localFileSystem, newFile) != 0)
                {
                    close(newFile->hostFd);
                    free(localName);
                    free(newFile);
                    return -1;
                }
                newFile->canRead = 0;
                newFile->canWrite = 1;
                newFile->name = localName;
                newFile->guestFd = guestSettings->nextGuestFd;
                guestSettings->nextGuestFd += 1;
                // printFileList(*localFileSystem);
                return newFile->guestFd;
            }
        }
        else
        {
            MyFile *newFile = (MyFile *)malloc(sizeof(MyFile));
            if (!newFile)
            {
                free(localName);
                return -1;
            }
            newFile->hostFd = open(localName, toRead ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC), S_IRWXU | S_IRWXG | S_IRWXO);
            if (newFile->hostFd < 0)
            {
                free(localName);
                free(newFile);
                return -1;
            }
            if (pushFile(localFileSystem, newFile) != 0)
            {
                close(newFile->hostFd);
                free(localName);
                free(newFile);
                return -1;
            }
            newFile->canRead = toRead;
            newFile->canWrite = 1 - toRead;
            newFile->name = localName;
            newFile->guestFd = guestSettings->nextGuestFd;
            guestSettings->nextGuestFd += 1;
            // printFileList(*localFileSystem);
            return newFile->guestFd;
        }
    }
}

static char closeFile(LinkedList **localFileSystem, int fd)
{
    LLNode *temp = *localFileSystem;
    MyFile *foundFile = NULL;
    while (temp)
    {
        MyFile *tempFile = (MyFile *)temp->data;
        if (tempFile->guestFd == fd)
        {
            foundFile = tempFile;
            break;
        }
        temp = temp->next;
    }
    if (!foundFile)
        return EOF;
    if (foundFile->hostFd < 0)
        return EOF;
    else
    {
        close(foundFile->hostFd);
        foundFile->guestFd = -1;
        foundFile->hostFd = -1;
        foundFile->canRead = 0;
        if (foundFile->canWrite)
        {
            foundFile->canWrite = 0;
        }
        else
        {
            deleteFile(localFileSystem, foundFile);
        }
    }
    return 0;
}

static char readFile(LinkedList *localFileSystem, int fd)
{
    LLNode *temp = localFileSystem;
    MyFile *foundFile = NULL;
    while (temp)
    {
        MyFile *tempFile = (MyFile *)temp->data;
        if (tempFile->guestFd == fd)
        {
            foundFile = tempFile;
            break;
        }
        temp = temp->next;
    }
    if (!foundFile)
        return EOF;
    if (!foundFile->canRead)
        return EOF;
    if (foundFile->hostFd < 0)
        return EOF;
    char buffer[1];
    int result = read(foundFile->hostFd, (void *)(&buffer), 1);
    if (result < 1)
        return EOF;
    return buffer[0];
}

static char writeFile(LinkedList *localFileSystem, int fd, char c, int id)
{
    LLNode *temp = localFileSystem;
    MyFile *foundFile = NULL;
    while (temp)
    {
        MyFile *tempFile = (MyFile *)temp->data;
        if (tempFile->guestFd == fd)
        {
            foundFile = tempFile;
            break;
        }
        temp = temp->next;
    }
    if (!foundFile)
        return EOF;
    if (!foundFile->canWrite)
        return EOF;
    if (foundFile->hostFd < 0)
        return EOF;
    char buffer[1] = {c};
    int result = write(foundFile->hostFd, (void *)(&buffer), 1);
    if (result < 1)
        return EOF;
    return c;
}

struct vm
{
    int vm_fd;
    int vcpu_fd;
    char *mem;
    struct kvm_run *kvm_run;
};

int init_vm(struct vm *vm, int kvm_fd, size_t mem_size)
{
    struct kvm_userspace_memory_region region;
    int kvm_run_mmap_size;

    vm->vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    if (vm->vm_fd < 0)
    {
        // perror("KVM_CREATE_VM");
        return -1;
    }

    vm->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vm->mem == MAP_FAILED)
    {
        // perror("mmap mem");
        return -1;
    }

    region.slot = 0;
    region.flags = 0;
    region.guest_phys_addr = 0;
    region.memory_size = mem_size;
    region.userspace_addr = (unsigned long)vm->mem;
    if (ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
    {
        // perror("KVM_SET_USER_MEMORY_REGION");
        return -1;
    }

    vm->vcpu_fd = ioctl(vm->vm_fd, KVM_CREATE_VCPU, 0);
    if (vm->vcpu_fd < 0)
    {
        // perror("KVM_CREATE_VCPU");
        return -1;
    }

    kvm_run_mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (kvm_run_mmap_size <= 0)
    {
        // perror("KVM_GET_VCPU_MMAP_SIZE");
        return -1;
    }

    vm->kvm_run = mmap(NULL, kvm_run_mmap_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, vm->vcpu_fd, 0);
    if (vm->kvm_run == MAP_FAILED)
    {
        // perror("mmap kvm_run");
        return -1;
    }

    return 0;
}

static void setup_64bit_code_segment(struct kvm_sregs *sregs)
{
    struct kvm_segment seg = {
        .base = 0,
        .limit = 0xffffffff,
        .present = 1, 
        .type = 11,   
        .dpl = 0,     
        .db = 0,      
        .s = 1,       
        .l = 1,       
        .g = 1,       
    };

    sregs->cs = seg;

    seg.type = 3; // Data: read, write, accessed
    sregs->ds = sregs->es = sregs->fs = sregs->gs = sregs->ss = seg;
}

static void setup_long_mode(struct vm *vm, struct kvm_sregs *sregs, int memorySize, int pageSize)
{
    uint64_t page = 0;
    uint64_t pml4_addr = 0x1000;
    uint64_t *pml4 = (void *)(vm->mem + pml4_addr);

    uint64_t pdpt_addr = 0x2000;
    uint64_t *pdpt = (void *)(vm->mem + pdpt_addr);

    uint64_t pd_addr = 0x3000;
    uint64_t *pd = (void *)(vm->mem + pd_addr);

    pml4[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pdpt_addr;
    pdpt[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pd_addr;

    if (pageSize == SIZE_4KB)
    {
        uint64_t pt_addr = 0x4000;
        uint64_t *pt = (void *)(vm->mem + pt_addr);

        int pdCount = memorySize / SIZE_2MB;
        // page = pt_addr + pdCount * SIZE_4KB;
        for (int i = 0; i < pdCount; i++)
        {
            pt_addr = 0x4000 + i * SIZE_4KB;
            pt = (void *)(vm->mem + pt_addr);
            pd[i] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pt_addr;
            for (int j = 0; j < 512; j++)
            {
                pt[j] = page | PDE64_PRESENT | PDE64_RW | PDE64_USER;
                page += SIZE_4KB;
            }
        }
    }
    else
    {
        for (int i = 0; i < memorySize / SIZE_2MB; i++)
        {
            pd[i] = page | PDE64_PRESENT | PDE64_RW | PDE64_USER | PDE64_PS;
            page += SIZE_2MB;
        }
    }
    sregs->cr3 = pml4_addr;
    sregs->cr4 = CR4_PAE;              
    sregs->cr0 = CR0_PE | CR0_PG;      
    sregs->efer = EFER_LME | EFER_LMA; 

    setup_64bit_code_segment(sregs);
}

static void *
runGuest(void *settings)
{
    GuestSettings *guestSettings = (GuestSettings *)settings;

    struct vm vm;
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    int stop = 0;
    int ret = 0;
    FILE *img;

    if (init_vm(&vm, guestSettings->kvmFd, guestSettings->memorySize))
    {
        printf("{Guest %d} Error: failed to init the VM \n", guestSettings->id);
        return (void *)-1;
    }

    if (ioctl(vm.vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
    {
        printf("{Guest %d} Error: KVM_GET_SREGS\n", guestSettings->id);
        return (void *)-1;
    }

    setup_long_mode(&vm, &sregs, guestSettings->memorySize, guestSettings->pageSize);

    if (ioctl(vm.vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
    {
        printf("{Guest %d} Error: KVM_SET_SREGS\n", guestSettings->id);
        return (void *)-1;
    }
    memset(&regs, 0, sizeof(regs));
    regs.rflags = 2;
    regs.rip = 0;
    regs.rsp = guestSettings->memorySize;

    if (ioctl(vm.vcpu_fd, KVM_SET_REGS, &regs) < 0)
    {
        printf("{Guest %d} Error: KVM_SET_REGS\n", guestSettings->id);
        return (void *)-1;
    }

    img = fopen(guestSettings->guestFile, "r");
    if (img == NULL)
    {
        printf("{Guest %d} Error: cannot open binary file\n", guestSettings->id);
        return (void *)-1;
    }

    char *p = vm.mem;
    while (feof(img) == 0)
    {
        int r = fread(p, 1, 1024, img);
        p += r;
    }
    fclose(img);

    LinkedList *sharedFileSystem = NULL;
    LinkedList *localFileSystem = NULL;
    for (LLNode *temp = guestSettings->sharedFiles; temp; temp = temp->next)
    {
        MyFile *file = (MyFile *)malloc(sizeof(MyFile));
        if (!file)
        {
            printf("{Guest %d} Error: malloc failed\n", guestSettings->id);
            deleteList(sharedFileSystem, 1);
            return (void *)-1;
        }
        file->hostFd = -1;
        file->guestFd = -1;
        file->name = (char *)temp->data;
        file->canRead = 1;
        file->canWrite = 0;
        if (pushFile(&sharedFileSystem, file) != 0)
        {
            printf("{Guest %d} Error: malloc failed\n", guestSettings->id);
            deleteList(sharedFileSystem, 1);
            return (void *)-1;
        }
    }

    int fileState1 = 0;
    int fileState2 = 0;
    int remainingBytes = 0;
    int fd = 0;
    char chr;
    char *filename = NULL;

    while (stop == 0)
    {
        ret = ioctl(vm.vcpu_fd, KVM_RUN, 0);
        if (ret == -1)
        {
            printf("{Guest %d} Error: KVM_RUN failed\n", guestSettings->id);
            return (void *)1;
        }

        switch (vm.kvm_run->exit_reason)
        {
        case KVM_EXIT_IO:
            if (vm.kvm_run->io.direction == KVM_EXIT_IO_OUT && vm.kvm_run->io.port == PORT_IO)
            {
                char *p = (char *)vm.kvm_run;
                printf("%c", *(p + vm.kvm_run->io.data_offset));
            }
            else if (vm.kvm_run->io.direction == KVM_EXIT_IO_IN && vm.kvm_run->io.port == PORT_IO)
            {
                char c;
                scanf("%c", &c);
                char *data_in = (((char *)vm.kvm_run) + vm.kvm_run->io.data_offset);
                *data_in = c;
            }
            else if (vm.kvm_run->io.direction == KVM_EXIT_IO_OUT && vm.kvm_run->io.port == PORT_FILE)
            {
                char *p = (char *)vm.kvm_run;
                char c = *(p + vm.kvm_run->io.data_offset);
                switch (fileState1)
                {
                case FSTATE1_OPEN_R:
                case FSTATE1_OPEN_W:
                    if (fileState2 == FSTATE2_FILENAME)
                    {
                        if (c == '\0')
                        {
                            if (!filename)
                            {
                                printf("{Guest %d} File system error - empty filename\n", guestSettings->id);
                                fileState1 = FSTATE1_NONE;
                                fileState2 = FSTATE2_NONE;
                            }
                            else
                            {
                                fileState2 = FSTATE2_FD;
                                remainingBytes = 4;
                                fd = openFile(&sharedFileSystem, &localFileSystem, filename, (fileState1 == FSTATE1_OPEN_R) ? 1 : 0, guestSettings);
                            }
                        }
                        else
                        {
                            char *newFilename = extendFilename(filename, c);
                            if (!newFilename)
                            {
                                free(filename);
                                printf("{Guest %d} File system error - failed to extend filename\n", guestSettings->id);
                                fileState1 = FSTATE1_NONE;
                                fileState2 = FSTATE2_NONE;
                            }
                            else
                            {
                                free(filename);
                                filename = newFilename;
                            }
                        }
                    }
                    else
                    {
                        printf("{Guest %d} File system error - undefined state1 + state2 combination\n", guestSettings->id);
                        fileState1 = FSTATE1_NONE;
                        fileState2 = FSTATE2_NONE;
                    }
                    break;
                case FSTATE1_CLOSE:
                    if (fileState2 == FSTATE2_FD)
                    {
                        remainingBytes -= 1;
                        fd |= ((unsigned int)c & 0xFF) << (remainingBytes * 8);
                        if (remainingBytes == 0)
                        {
                            fileState2 = FSTATE2_CHAR;
                            chr = closeFile(&localFileSystem, fd);
                        }
                    }
                    else
                    {
                        printf("{Guest %d} File system error - undefined state1 + state2 combination\n", guestSettings->id);
                        fileState1 = FSTATE1_NONE;
                        fileState2 = FSTATE2_NONE;
                    }
                    break;
                case FSTATE1_READ:
                    if (fileState2 == FSTATE2_FD)
                    {
                        remainingBytes -= 1;
                        fd |= ((unsigned int)c & 0xFF) << (remainingBytes * 8);
                        if (remainingBytes == 0)
                        {
                            fileState2 = FSTATE2_CHAR;
                            chr = readFile(localFileSystem, fd);
                        }
                    }
                    else
                    {
                        printf("{Guest %d} File system error - undefined state1 + state2 combination\n", guestSettings->id);
                        fileState1 = FSTATE1_NONE;
                        fileState2 = FSTATE2_NONE;
                    }
                    break;
                case FSTATE1_WRITE:
                    if (fileState2 == FSTATE2_FD)
                    {
                        remainingBytes -= 1;
                        fd |= ((unsigned int)c & 0xFF) << (remainingBytes * 8);
                        if (remainingBytes == 0)
                        {
                            fileState2 = FSTATE2_CHAR;
                        }
                    }
                    else if (fileState2 == FSTATE2_CHAR)
                    {
                        chr = c;
                        fileState1 = FSTATE1_READ;
                        chr = writeFile(localFileSystem, fd, chr, guestSettings->id);
                    }
                    break;
                case FSTATE1_NONE:
                    switch (c)
                    {
                    case FILE_OPEN_R:
                        fileState1 = FSTATE1_OPEN_R;
                        fileState2 = FSTATE2_FILENAME;
                        filename = NULL;
                        break;
                    case FILE_OPEN_W:
                        fileState1 = FSTATE1_OPEN_W;
                        fileState2 = FSTATE2_FILENAME;
                        filename = NULL;
                        break;
                    case FILE_CLOSE:
                        fileState1 = FSTATE1_CLOSE;
                        fileState2 = FSTATE2_FD;
                        fd = 0;
                        remainingBytes = 4;
                        break;
                    case FILE_READ:
                        fileState1 = FSTATE1_READ;
                        fileState2 = FSTATE2_FD;
                        fd = 0;
                        remainingBytes = 4;
                        break;
                    case FILE_WRITE:
                        fileState1 = FSTATE1_WRITE;
                        fileState2 = FSTATE2_FD;
                        fd = 0;
                        remainingBytes = 4;
                        break;
                    default:
                        printf("{Guest %d} File system error - undefined syscall code\n", guestSettings->id);
                    }
                    break;
                default:
                    printf("{Guest %d} File system error - undefined state1 + state2 combination\n", guestSettings->id);
                    fileState1 = FSTATE1_NONE;
                    fileState2 = FSTATE2_NONE;
                }
            }
            else if (vm.kvm_run->io.direction == KVM_EXIT_IO_IN && vm.kvm_run->io.port == PORT_FILE)
            {
                char *data_in = (((char *)vm.kvm_run) + vm.kvm_run->io.data_offset);
                switch (fileState1)
                {
                case FSTATE1_OPEN_R:
                case FSTATE1_OPEN_W:
                    if (fileState2 == FSTATE2_FD)
                    {
                        remainingBytes -= 1;
                        uint8_t byte = (uint8_t)((fd >> (8 * remainingBytes)) & 0xFF);
                        *data_in = (char)(byte);
                        if (remainingBytes == 0)
                        {
                            fileState1 = FSTATE1_NONE;
                            fileState2 = FSTATE2_NONE;
                        }
                    }
                    else
                    {
                        printf("{Guest %d} File system error - undefined behaviour\n", guestSettings->id);
                        fileState1 = FSTATE1_NONE;
                        fileState2 = FSTATE2_NONE;
                    }
                    break;
                case FSTATE1_CLOSE:
                case FSTATE1_READ:
                    if (fileState2 == FSTATE2_CHAR)
                    {
                        *data_in = chr;
                        fileState1 = FSTATE1_NONE;
                        fileState2 = FSTATE2_NONE;
                    }
                    else
                    {
                        printf("{Guest %d} File system error - undefined behaviour\n", guestSettings->id);
                        fileState1 = FSTATE1_NONE;
                        fileState2 = FSTATE2_NONE;
                    }
                    break;
                default:
                    printf("{Guest %d} File system error - undefined behaviour\n", guestSettings->id);
                    fileState1 = FSTATE1_NONE;
                    fileState2 = FSTATE2_NONE;
                }
            }
            break;
        case KVM_EXIT_HLT:
            printf("{Guest %d} KVM_EXIT_HLT\n", guestSettings->id);
            stop = 1;
            break;
        case KVM_EXIT_INTERNAL_ERROR:
            printf("{Guest %d} Error: internal error = 0x%x\n", guestSettings->id, vm.kvm_run->internal.suberror);
            stop = 1;
            break;
        case KVM_EXIT_SHUTDOWN:
            printf("{Guest %d} Shutdown\n", guestSettings->id);
            stop = 1;
            break;
        default:
            printf("{Guest %d} Exit reason: %d\n", guestSettings->id, vm.kvm_run->exit_reason);
            stop = 1;
            break;
        }
    }
    deleteFileList(&sharedFileSystem);
    deleteFileList(&localFileSystem);
    return (void *)0;
}

int isDigit(char *s)
{
    if (*s < '0' || *s > '9' || *(s + 1))
        return 0;
    else
        return 1;
}

int main(int argc, char **argv)
{
    int kvmFd = open("/dev/kvm", O_RDWR);
    if (kvmFd < 0)
    {
        printf("Error: failed to open /dev/kvm");
        return -1;
    }
    int memorySize, pageSize;
    char memorySet = 0, pageSet = 0; // 0, 1, 2
    char guestSet = 0;               // 0, 1, 2, 3
    int guestCount = 0;
    char sharedSet = 0; // 0, 1, 2, 3
    int sharedCount = 0;
    LinkedList *guestFilenames = NULL;
    LinkedList *sharedFilenames = NULL;
    LLNode *temp;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--memory") == 0 || strcmp(argv[i], "-m") == 0)
        {
            if (guestSet == 1 || pageSet == 1 || sharedSet == 1 || memorySet > 0)
            {
                printf("Error: bad command line arguments\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            if (guestSet == 2)
                guestSet = 3;
            if (sharedSet == 2)
                sharedSet == 3;
            memorySet = 1;
        }
        else if (strcmp(argv[i], "--page") == 0 || strcmp(argv[i], "-p") == 0)
        {
            if (guestSet == 1 || memorySet == 1 || sharedSet == 1 || pageSet > 0)
            {
                printf("Error: bad command line arguments\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            if (guestSet == 2)
                guestSet = 3;
            if (sharedSet == 2)
                sharedSet == 3;
            pageSet = 1;
        }
        else if (strcmp(argv[i], "--guest") == 0 || strcmp(argv[i], "-g") == 0)
        {
            if (memorySet == 1 || pageSet == 1 || sharedSet == 1 || guestSet > 0)
            {
                printf("Error: bad command line arguments\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            if (sharedSet == 2)
                sharedSet == 3;
            guestSet = 1;
        }
        else if (strcmp(argv[i], "--file") == 0 || strcmp(argv[i], "-f") == 0)
        {
            if (memorySet == 1 || pageSet == 1 || guestSet == 1 || sharedSet > 0)
            {
                printf("Error: bad command line arguments\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            if (guestSet == 2)
                guestSet = 3;
            sharedSet = 1;
        }
        else if (memorySet == 1)
        {
            if (!isDigit(argv[i]))
            {
                printf("Error: bad --memory argument\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            int m = atoi(argv[i]);
            if (m != 2 && m != 4 && m != 8)
            {
                printf("Error: bad --memory argument\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            memorySize = m * 0x100000;
            memorySet = 2;
        }
        else if (pageSet == 1)
        {
            if (!isDigit(argv[i]))
            {
                printf("Error: bad --page argument\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            int p = atoi(argv[i]);
            if (p != 2 && p != 4)
            {
                printf("Error: bad --page argument\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            pageSize = (p == 2) ? 0x200000 : 0x1000;
            pageSet = 2;
        }
        else if (guestSet == 1 || guestSet == 2)
        {
            int l = strlen(argv[i]);
            if (l > 200)
            {
                printf("Error: guest file's name length must be less than or equal to 200\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            if (pushString(&guestFilenames, argv[i]) != 0)
            {
                printf("Error: malloc failed\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            guestSet = 2;
            guestCount++;
        }
        else if (sharedSet == 1 || sharedSet == 2)
        {
            int l = strlen(argv[i]);
            if (l > 300)
            {
                printf("Error: path to shared file mustn't be longer than %d characters\n", 300);
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            if (pushString(&sharedFilenames, argv[i]) != 0)
            {
                printf("Error: malloc failed\n");
                deleteList(guestFilenames, 1);
                deleteList(sharedFilenames, 1);
                return -1;
            }
            sharedSet = 2;
            sharedCount++;
        }
        else
        {
            printf("Error: bad command line arguments\n");
            deleteList(guestFilenames, 1);
            deleteList(sharedFilenames, 1);
            return -1;
        }
    }
    if (memorySet < 2 || pageSet < 2 || guestSet < 2 || sharedSet == 1)
    {
        printf("Bad command line arguments\n");
        deleteList(guestFilenames, 1);
        deleteList(sharedFilenames, 1);
        return -1;
    }
    GuestSettings *settingsArr = (GuestSettings *)malloc(guestCount * sizeof(GuestSettings));
    if (settingsArr == NULL)
    {
        printf("Error: malloc failed\n");
        deleteList(guestFilenames, 1);
        deleteList(sharedFilenames, 1);
        return -1;
    }
    temp = guestFilenames;
    for (int i = 0; i < guestCount; i++)
    {
        settingsArr[i].memorySize = memorySize;
        settingsArr[i].pageSize = pageSize;
        settingsArr[i].guestFile = temp->data;
        settingsArr[i].kvmFd = kvmFd;
        settingsArr[i].id = i;
        settingsArr[i].sharedFileCount = sharedCount;
        settingsArr[i].sharedFiles = sharedFilenames;
        settingsArr[i].nextGuestFd = 0;
        temp = temp->next;
    }
    deleteList(guestFilenames, 0);
    pthread_t *threads = (pthread_t *)malloc(guestCount * sizeof(pthread_t));
    if (threads == NULL)
    {
        printf("Error: malloc failed\n");
        for (int i = 0; i < guestCount; i++)
            free(settingsArr[i].guestFile);
        free(settingsArr);

        deleteList(sharedFilenames, 1);
        return -1;
    }
    for (int i = 0; i < guestCount; i++)
    {
        pthread_create(&threads[i], NULL, &runGuest, &settingsArr[i]);
    }
    for (int i = 0; i < guestCount; i++)
    {
        pthread_join(threads[i], NULL);
        free(settingsArr[i].guestFile);
    }
    free(settingsArr);
    free(threads);
    deleteList(sharedFilenames, 1);
    printf("\nProgram successfully closed\n");
    return 0;
}