#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fs/defines.h>

char buf[8192];
char name[3];

void opentest(void)
{
    int fd;

    printf("open test\n");
    fd = open("echo", 0);
    if (fd < 0) {
        printf("open echo failed!\n");
        exit(1);
    }
    close(fd);
    fd = open("doesnotexist", 0);
    if (fd >= 0) {
        printf("open doesnotexist succeeded!\n");
        exit(0);
    }
    printf("open test ok\n");
}

void writetest(void)
{
    int fd;
    int i;

    printf("small file test\n");
    fd = open("small", O_CREAT | O_RDWR);
    if (fd >= 0) {
        printf("creat small succeeded; ok\n");
    } else {
        printf("error: creat small failed!\n");
        exit(1);
    }
    for (i = 0; i < 100; i++) {
        if (write(fd, "aaaaaaaaaa", 10) != 10) {
            printf("error: write aa %d new file failed\n", i);
            exit(1);
        }
        if (write(fd, "bbbbbbbbbb", 10) != 10) {
            printf("error: write bb %d new file failed\n", i);
            exit(1);
        }
    }
    printf("writes ok\n");
    close(fd);
    fd = open("small", O_RDONLY);
    if (fd >= 0) {
        printf("open small succeeded ok\n");
    } else {
        printf("error: open small failed!\n");
        exit(1);
    }
    i = read(fd, buf, 2000);
    if (i == 2000) {
        printf("read succeeded ok\n");
    } else {
        printf("read failed\n");
        exit(1);
    }
    close(fd);

    if (unlink("small") < 0) {
        printf("unlink small failed\n");
        exit(1);
    }
    printf("small file test ok\n");
}

void writetestbig(void)
{
    int i, fd, n;

    printf("big files test\n");

    fd = open("big", O_CREAT | O_RDWR);
    if (fd < 0) {
        printf("error: creat big failed!\n");
        exit(1);
    }

    for (i = 0; i < INODE_MAX_BLOCKS; i++) {
        ((int *)buf)[0] = i;
        if (write(fd, buf, 512) != 512) {
            printf("error: write big file failed\n");
            exit(1);
        }
    }

    close(fd);

    fd = open("big", O_RDONLY);
    if (fd < 0) {
        printf("error: open big failed!\n");
        exit(1);
    }

    n = 0;
    for (;;) {
        i = read(fd, buf, 512);
        if (i == 0) {
            if (n == INODE_MAX_BLOCKS - 1) {
                printf("read only %d blocks from big", n);
                exit(1);
            }
            break;
        } else if (i != 512) {
            printf("read failed %d\n", i);
            exit(1);
        }
        if (((int *)buf)[0] != n) {
            printf("read content of block %d is %d\n", n, ((int *)buf)[0]);
            exit(1);
        }
        n++;
    }
    close(fd);
    if (unlink("big") < 0) {
        printf("unlink big failed\n");
        exit(1);
    }
    printf("big files ok\n");
}

void createtest(void)
{
    int i, fd;

    printf("many creates, followed by unlink test\n");

    name[0] = 'a';
    name[2] = '\0';
    for (i = 0; i < 52; i++) {
        name[1] = '0' + i;
        fd = open(name, O_CREAT | O_RDWR);
        close(fd);
    }
    name[0] = 'a';
    name[2] = '\0';
    for (i = 0; i < 52; i++) {
        name[1] = '0' + i;
        unlink(name);
    }
    printf("many creates, followed by unlink; ok\n");
}
void heredoc_test(void)
{
    int fd;
    const char *heredoc_content = "This is line 1\nThis is line 2\nThis is line 3\n";
    const char *expected_content = "This is line 1\nThis is line 2\nThis is line 3\n";

    printf("Here Document test\n");

    // Simulate writing Here Document to a file
    fd = open("heredoc_test", O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        printf("error: creat heredoc_test failed!\n");
        exit(1);
    }

    if (write(fd, heredoc_content, strlen(heredoc_content)) != (ssize_t)strlen(heredoc_content)) {
        printf("error: write heredoc_test failed!\n");
        exit(1);
    }
    close(fd);

    // Read back the file and verify content
    fd = open("heredoc_test", O_RDONLY);
    if (fd < 0) {
        printf("error: open heredoc_test failed!\n");
        exit(1);
    }

    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, sizeof(buf)) <= 0) {
        printf("error: read heredoc_test failed!\n");
        exit(1);
    }
    close(fd);

    // Verify the content matches
    if (strcmp(buf, expected_content) != 0) {
        printf("error: heredoc content mismatch!\n");
        printf("expected:\n%s\n", expected_content);
        printf("got:\n%s\n", buf);
        exit(1);
    }

    // Cleanup
    if (unlink("heredoc_test") < 0) {
        printf("unlink heredoc_test failed\n");
        exit(1);
    }

    printf("Here Document test ok\n");
}
int main(int argc, char *argv[])
{
    printf("usertests starting\n");

    opentest();
    writetest();
    writetestbig();
    createtest();
    heredoc_test(); // Added Here Document test
    printf("usertests ok\n");
    exit(0);
}