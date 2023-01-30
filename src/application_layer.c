// Application layer protocol implementation

#include "application_layer.h"

// includurile mele

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link_layer.h"

// defineurile mele

#define BUFSIZE 512
#define K 128
#define PACKET_DATA_SIZE 128
#define C_START 0x02
#define C_DATA 0x01
#define C_END 0x03
#define T_SIZE 0x00
#define T_NAME 0x01

//-----------function definitions------------
// int data_packet face un packet cu k bytes de informatie din fisier +
// toate campurile necesare

int data_packet(FILE *file_fd, unsigned char *buf, unsigned char N) {
    buf[0] = C_DATA;
    buf[1] = N;

    int bytes_read = fread(buf + 4, 1, K, file_fd);
    buf[2] = bytes_read / 256;
    buf[3] = bytes_read - buf[2];
    // printf("BYTES READ: %d\n", bytes_read);
    int data_size = bytes_read + 4;

    return data_size;
}

// int control_packet formeaza un packet de control de tipul START sau STOP, si
// intoarce lungimea acestui packet
int control_packet(FILE *file_fd, unsigned char *buf, int c_flag,
                   const char *pathname) {
    buf[0] = c_flag;
    //-----Size of the file--------
    buf[1] = T_SIZE;

    fseek(file_fd, 0, SEEK_END);
    long size = ftell(file_fd);
    long copy_size = size;
    fseek(file_fd, 0, SEEK_SET);

    // calculare L, nr. de digits
    int L = 0;
    while (size != 0) {
        size = size / 10;
        L++;
    }
    sprintf(buf + 2, "%c", L);
    sprintf(buf + 3, "%ld", copy_size);
    // for(int i = 0; i < 10; i++)
    //     printf("%d ", buf[i]);

    //--------------------------------------
    //--------Name of the file-------------

    buf[3 + L] = T_NAME;
    buf[4 + L] = strlen(pathname);
    strcpy(buf + 5 + L, pathname);

    return 5 + L + strlen(pathname);
}

int sendFile(int connection_fd, const char *pathname, LinkLayer link_struct) {
    FILE *file_fd = fopen(pathname, "rb");
    if (file_fd == NULL) {
        perror("The file that you want to send was not opened succesfully\n");
        exit(-1);
    }
    printf("File opened succesfully!\n\n");

    unsigned char buf[BUFSIZE] = {0};

    int control_size = control_packet(file_fd, buf, C_START, pathname);
    // for (int i = 0; i < control_size; i++) printf("%d ", buf[i]);

    //-----------------------------------------
    //-----Trimite primul llwrite()------------

    int ok = llwrite(connection_fd, buf, control_size, link_struct, 0);
    if (ok == 0) {
        perror("Control Packet not sent\n");
        exit(-1);
    }


    //-------------------------------------

    unsigned char N = 0;
    fseek(file_fd, 0, SEEK_SET);
    int color = 1;
    ok = 1;
    while (!feof(file_fd) && ok != 0) {
        // unsigned char newbuff[BUFSIZE] = {0};
        int data_size = data_packet(file_fd, buf, N);
        ok = llwrite(connection_fd, buf, data_size, link_struct, color % 2);
        printf("%d bytes of data sent.\nCursor -> %d , FEOF? -> %d\n", ok, ftell(file_fd), feof(file_fd));

        color++;
        N++;
    }
    if ( ok == 0 ) { return -1;}

    control_size = control_packet(file_fd, buf, C_END, pathname);
    ok = llwrite(connection_fd, buf, control_size, link_struct, color % 2);
    if ( ok == 0 ) { return -1;}

    return 1;
}

int recvFile(int connection_fd) {
    unsigned char buf[BUFSIZE] = {0};
    unsigned char filename_start[BUFSIZE] = {0};
    unsigned char filename_end[BUFSIZE] = {0};

    FILE *new_fd;
    int filesize_start = 0;
    int filesize_end = 0;

    int counter, L1, L2, k;
    int color = 0;
    int ok_read = 1;
    int N = -1;
    unsigned char length1 = 0;
    unsigned char length2 = 0;

    //----first llread() is speacial, it contains metainformations about the
    // file

    while (ok_read) {
        int bytes = llread(connection_fd, buf, color % 2);
        
        // for ( int i=0 ; i < bytes; i++)
        // {
        //     printf( "buf[%d]  = %x\n", i , buf[i]);
        // }
        switch (buf[0]) {
            case 0: printf( "SETFRAME sent again\n");
            case 2:
                printf("START\n");
                
                if (buf[1] == T_SIZE) {
                    length1 = buf[2];
                    filesize_start = atoi(buf + 3);
                    printf("filesize=%d bytes\n\n", filesize_start);
                } else
                    return -1;
                if (buf[3 + length1] == T_NAME) {
                    length2 = buf[4 + length1];
                    strncpy(filename_start, buf + 5 + length1, length2);
                    filename_start[length2] = '\0';
                    strcat(filename_start, "_received.gif");
                    //printf( "length2=%d\nfilename=%s\n", length2 , filename_start);

                    new_fd = fopen(filename_start, "wb");
                    if(new_fd == NULL)
                        printf("Could not create a received file!\n");
                } else
                    return -1;

                color++;
                break;
            case 1:
                printf("INFO\n");
                counter = buf[1];
                if (counter != N + 1) {
                    perror("Counter invalid\n");
                    return -1;
                }
                N++;
                L1 = buf[2];
                L2 = buf[3];
                k = 256 * L1 + L2;

                fwrite(buf + 4 , 1 , k , new_fd);
                printf( "Cursor -> %d\n\n", ftell(new_fd));
                color++;
                break;
            case 3:
                printf("END\n\n");
                if (buf[1] == T_SIZE) {
                    length1 = buf[2];
                    filesize_end = atoi(buf + 3);
                } else
                    return -1;
                if (buf[3 + length1] == T_NAME) {
                    length2 = buf[4 + length1];
                    strncpy(filename_end, buf + 5 + length1, length2);
                    filename_end[length2] = '\0';
                    strcat(filename_end, "_received.gif");

                    if (strcmp(filename_end, filename_start) != 0 ||
                        (filesize_start != filesize_end)) {
                        return -1;
                    }
                    fclose(new_fd);
                }
                color++;
                ok_read = 0;
                break;

            default:
                printf("UNKNOWN TYPE OF FRAME\n");
                return -1;
        }
    }

    return 1;
}

void applicationLayer(const char *serialPort, const char *role, int baudRate,
                      int nTries, int timeout, const char *filename) {
    /*-----Creare structura LinkLayer si setarea campurilor----*/

    LinkLayer link_struct;

    strcpy(link_struct.serialPort, serialPort);

    if (strcmp(role, "tx") == 0) {
        link_struct.role = LlTx;
    } else if (strcmp(role, "rx") == 0) {
        link_struct.role = LlRx;
    } else {
        perror("Invalid role\n");
        exit(-1);
    }

    link_struct.baudRate = baudRate;
    link_struct.nRetransmissions = nTries;
    link_struct.timeout = timeout;

    /*------Stabilirea legaturii prin functia llopen()------*/

    int connection_fd = llopen(link_struct);
    if (connection_fd <= 0) {
        perror("Could not establish connection!\n");
        exit(-1);
    }

    if (link_struct.role == LlTx) {
        sendFile(connection_fd, filename, link_struct);
    }
    if (link_struct.role == LlRx) {
        recvFile(connection_fd);
    }
    int ok = llclose(connection_fd, link_struct, FALSE);
    if(ok == -1) {
        perror("Connection NOT closed!\n");
        exit(-1);
    }
    printf("Connection ended successfully!\n");
    
}
