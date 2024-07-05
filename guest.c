#include <stddef.h>
#include <stdint.h>

const char EOF = -1;

const uint16_t PORT_IO = 0x00E9;
const uint16_t PORT_FILE = 0x0278;

const int MAX_PATH_LENGTH = 300;

const uint8_t FILE_OPEN_R = 0x1;
const uint8_t FILE_OPEN_W = 0x2;
const uint8_t FILE_CLOSE = 0x3;
const uint8_t FILE_READ = 0x4;
const uint8_t FILE_WRITE = 0x5;


static void outb(uint16_t port, uint8_t value)
{
    asm("outb %0,%1" : /* empty */ : "a"(value), "Nd"(port) : "memory");
}

static uint8_t inb(uint16_t port)
{
    uint8_t value;
    asm(
        "inb %1, %0"
        : "=a"(value) // Output: store the result in the variable pointed to by value
        : "Nd"(port)  // Input: the port number to read from
    );
    return value;
}

static void putchar(char c)
{
    outb(PORT_IO, (uint8_t)c);
}

static char getchar()
{
    return (char)inb(PORT_IO);
}

static int fopen(const char *s, char mode)
{
    if (!s || strlen(s) == 0 || strlen(s) > MAX_PATH_LENGTH)
        return -1;
    if (mode == 'r')
        outb(PORT_FILE, FILE_OPEN_R);
    else if (mode == 'w')
        outb(PORT_FILE, FILE_OPEN_W);
    else
        return -1;
    for (const char *p = s; *p; p++)
    {
        outb(PORT_FILE, (uint8_t)(*p));
    }
    outb(PORT_FILE, 0);
    uint8_t retBytes[4];
    unsigned int ret = 0;
    retBytes[0] = inb(PORT_FILE);
    retBytes[1] = inb(PORT_FILE);
    retBytes[2] = inb(PORT_FILE);
    retBytes[3] = inb(PORT_FILE);
    ret |= ((unsigned int)retBytes[0]) << 24;
    ret |= ((unsigned int)retBytes[1]) << 16;
    ret |= ((unsigned int)retBytes[2]) << 8;
    ret |= (unsigned int)retBytes[3];
    return (int)(ret);
}

static int fclose(int fd)
{
    if (fd < 0)
        return -1;
    unsigned int ufd = (unsigned int)fd;
    outb(PORT_FILE, FILE_CLOSE);
    outb(PORT_FILE, (uint8_t)((ufd >> 24) & 0xFF));
    outb(PORT_FILE, (uint8_t)((ufd >> 16) & 0xFF));
    outb(PORT_FILE, (uint8_t)((ufd >> 8) & 0xFF));
    outb(PORT_FILE, (uint8_t)(ufd & 0xFF));
    uint8_t ret = inb(PORT_FILE);
    return (ret == 0) ? 0 : -1;
}

static char fread(int fd)
{
    if (fd < 0)
        return -1;
    unsigned int ufd = (unsigned int)fd;
    outb(PORT_FILE, FILE_READ);
    outb(PORT_FILE, (uint8_t)((ufd >> 24) & 0xFF));
    outb(PORT_FILE, (uint8_t)((ufd >> 16) & 0xFF));
    outb(PORT_FILE, (uint8_t)((ufd >> 8) & 0xFF));
    outb(PORT_FILE, (uint8_t)(ufd & 0xFF));
    uint8_t ret = inb(PORT_FILE);
    return (char)ret;
}

static char fwrite(char c, int fd)
{
    if (fd < 0)
        return -1;
    unsigned int ufd = (unsigned int)fd;
    outb(PORT_FILE, FILE_WRITE);
    outb(PORT_FILE, (uint8_t)((ufd >> 24) & 0xFF));
    outb(PORT_FILE, (uint8_t)((ufd >> 16) & 0xFF));
    outb(PORT_FILE, (uint8_t)((ufd >> 8) & 0xFF));
    outb(PORT_FILE, (uint8_t)(ufd & 0xFF));
    outb(PORT_FILE, (uint8_t)c);
    uint8_t ret = inb(PORT_FILE);
    return (char)ret;
}

void
    __attribute__((noreturn))
    __attribute__((section(".start")))
    _start(void)
{

    /*
        INSERT CODE BELOW THIS LINE
    */

	const char* str="Hello world!";
	const char* p=str;
	while(true){
	    putchar(*p);
            if(*p) p++;
            else break;
	}

    /*
        INSERT CODE ABOVE THIS LINE
    */
    for (;;)
        asm("hlt");
}
