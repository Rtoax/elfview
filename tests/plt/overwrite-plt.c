#include <unistd.h>

#if OVERWRITE==1
ssize_t write(int fd, const void *buf, size_t count)
{
	return 0;
}
#endif

int main()
{
	return write(0, NULL, 0);
}
