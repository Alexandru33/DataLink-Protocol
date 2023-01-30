// Link layer protocol implementation

#include "link_layer.h"

// includurile mele

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

// MISC
#define _POSIX_SOURCE 1  // POSIX compliant source

// defineurile mele

#define BAUDRATE B38400

/*----pentru llopen()------*/
#define F 0x7e
#define A_SET 0x03
#define A_UA 0x03  // UA nu e frame de Command, vezi slide 10 din PDF
#define C_SET 0x03
#define C_UA 0x07
#define BUF_SIZE 256

/*------pentru llwrite()----------*/

#define A_WRITE 0x03
#define C_WHITE 0x00
#define C_BLACK 0x40
#define ESC 0x7d

#define A_RR 0x03
#define C_RR_0 0x05
#define C_RR_1 0x85

#define A_REJ 0x03
#define C_REJ_0 0x01
#define C_REJ_1 0x81

#define A_DISC_TX 0x03
#define C_DISC 0x0b

#define A_DISC_RX 0x01
#define SEND_SIZE 512

volatile int STOP = FALSE;

int alarmCount = 0;
int alarmEnabled = FALSE;

void alarmHandler(int signal) {
    alarmEnabled = FALSE;
    alarmCount++;

    STOP = TRUE;

    printf("Alarm #%d\n", alarmCount);
}
/*------------------------- */

////////////////////////////////////////////////
// SEND RECEIVER_READY
////////////////////////////////////////////////
void send_rr(int color, int connection_fd) {
    unsigned char buf[BUF_SIZE] = {0};
    buf[0] = F;
    buf[1] = A_RR;
    if (color == 0)
        buf[2] = C_RR_0;
    else
        buf[2] = C_RR_1;
    buf[3] = buf[1] ^ buf[2];
    buf[4] = F;

    int bytes = write(connection_fd, buf, 5);

    if (bytes == -1) {
        perror("Write error in send_rr()\n");
        exit(-1);
    }
    // printf("%d bytes sent by sendrr()\n", bytes);
}

////////////////////////////////////////////////
// SEND REJECTED
////////////////////////////////////////////////
void send_rej(int color, int connection_fd) {
    unsigned char buf[BUF_SIZE] = {0};
    buf[0] = F;
    buf[1] = A_REJ;
    if (color == 0)
        buf[2] = C_REJ_0;
    else
        buf[2] = C_REJ_1;
    buf[3] = buf[1] ^ buf[2];
    buf[4] = F;

    int bytes = write(connection_fd, buf, 5);

    if (bytes == -1) {
        perror("Write error in send_rr()\n");
        exit(-1);
    }
}

////////////////////////////////////////////////
// STUFFING
////////////////////////////////////////////////
int stuffing(unsigned char* buf, int buf_size) {
    unsigned char stuffed_buf[SEND_SIZE] = {0};
    stuffed_buf[0] = buf[0];  // primul flag F

    int i = 1;
    int occ = 0;  // number of 0x7d or 0x7e occurences

    while (i < buf_size - 1) {
        if (buf[i] != F && buf[i] != ESC) {
            stuffed_buf[i + occ] = buf[i];
            i++;
        } else {
            stuffed_buf[i + occ] = ESC;
            stuffed_buf[i + occ + 1] = buf[i] ^ 0x20;
            i++;
            occ++;
        }
    }

    stuffed_buf[i + occ] = buf[i];  // ultimul flag f

    buf_size = buf_size + occ;  // actualizare buf_size

    memcpy(buf, stuffed_buf, buf_size);

    return buf_size;
}
////////////////////////////////////////////////
// DESTUFFING
////////////////////////////////////////////////
int destuffing(unsigned char* buf, int buf_size) {
    unsigned char destuffed_buf[SEND_SIZE] = {0};
    destuffed_buf[0] = buf[0];  // primul flag F

    int i = 1;
    int occ = 0;

    while (i < buf_size - 1) {
        if (buf[i] != ESC) {
            destuffed_buf[i - occ] = buf[i];
            i++;
        } else if (buf[i] == ESC) {
            if (buf[i + 1] == 0x5e)
                destuffed_buf[i - occ] = F;
            else if (buf[i + 1] == 0x5d)
                destuffed_buf[i - occ] = ESC;
            else
                return -1;
            i = i + 2;
            occ++;
        } else
            i++;
    }

    destuffed_buf[i - occ] = buf[i];
    buf_size = buf_size - occ;
    memcpy(buf, destuffed_buf, buf_size);

    return buf_size;
}

////////////////////////////////////////////////
// CHECKING A RECEIVED FRAME, AFTER DESTUFFING
////////////////////////////////////////////////
// return 0 when packet is malformed
// return 1 when eveerything is ok
// return 2 when I received a duplicated frame
// return 3 when I received a SET frame (maybe from an UA lost on the way to TX)

int check_received_frame(unsigned char* buf, int bufsize, int expected_color) {
    // printf( "expect color =%d si bufsize=%d\n", expected_color, bufsize);

    if (bufsize == 5) {
        if (buf[0] == F && buf[1] == A_SET && buf[2] == C_SET &&
            buf[3] == (buf[1] ^ buf[2]) && buf[4] == F)
            return 3;
    }
    if (buf[0] != F) return 0;
    if (buf[bufsize - 1] != F) return 0;
    if (buf[1] != A_WRITE) return 0;
    if (buf[2] != C_BLACK && buf[2] != C_WHITE) return 0;
    if (buf[2] == C_BLACK && expected_color == 0) return 2;
    if (buf[2] == C_WHITE && expected_color == 1) return 2;
    if (buf[3] != (buf[2] ^ buf[1])) {
        printf("Error at BCC1\n");
        return 0;
    }
    // checking if bcc2 is equalto "xor" applied to all characters
    unsigned char bcc2 = buf[4];
    for (int i = 5; i < bufsize - 2; i++) {
        bcc2 = bcc2 ^ buf[i];
    }
    if (bcc2 != buf[bufsize - 2]) {
        printf("Error at BCC2\n");
        return 0;
    }

    return 1;
}
////////////////////////////////////////////////
// LLOPEN_TRANSMITTER
////////////////////////////////////////////////
int llopen_tx(LinkLayer transmitter, int fd) {
    (void)signal(SIGALRM, alarmHandler);

    while (alarmCount <= transmitter.nRetransmissions) {
        STOP = FALSE;

        if (alarmEnabled == FALSE) {
            unsigned char bcc_set = A_SET ^ C_SET;
            unsigned char set[5] = {F, A_SET, C_SET, bcc_set, F};

            int bytes = write(fd, set, 5);
            if (bytes != 5) {
                perror("SETFRAME (5 bytes) not sent\n");
                exit(-1);
            }
            alarm(transmitter.timeout);
            alarmEnabled = TRUE;

            // Am trimis set-ul si incep cornometrul si citire de pe teava
            // alarmHandler-ul imi face STOP = TRUE daca a trecut timeout-ul

            unsigned char buf[BUF_SIZE] = {0};

            int i = 0;
            while (STOP == FALSE) {
                int bytes = read(fd, buf + i, 1);
                if (bytes > 1) {
                    perror("Invalid reading in llopen_tx()!\n");
                    continue;
                }

                if (i == 0) {
                    if (buf[i] == F) {
                        i++;
                    }
                    continue;
                }

                if (i == 1) {
                    if (buf[i] != F) {
                        i++;
                    }
                    continue;
                }

                if (i > 1) {
                    if (buf[i] != F) {
                        i++;
                        continue;
                    } else {
                        if (i == 4) {
                            unsigned char bcc_ua = buf[1] ^ buf[2];
                            if (bcc_ua == buf[3]) {
                                if (buf[2] != C_UA) {
                                    i = 0;
                                    continue;
                                }
                                //printf("llopen_tx() succesfull\n");
                                return 1;

                            } else {
                                i = 0;
                                continue;
                            }
                        } else {
                            i = 0;
                            continue;
                        }
                    }
                }
            }
            // for (int j = 0; j < 5; j++) {
            //     printf("%x\n", buf[j]);
            // }
        }
    }

    printf("llopen_tx() unsuccesful\n");
    return 0;
}

////////////////////////////////////////////////
// LLOPEN_RECEIVER
////////////////////////////////////////////////
int llopen_rx(LinkLayer receiver, int fd) {
    unsigned char buf[BUF_SIZE] = {0};

    int i = 0;
    while (STOP == FALSE) {
        int bytes = read(fd, buf + i, 1);
        if (bytes > 1) {
            perror("Invalid reading in  llopen_rx()!\n");
            continue;
        }

        if (i == 0) {
            if (buf[i] == F) {
                i++;
            }
            continue;
        }

        if (i == 1) {
            if (buf[i] != F) {
                i++;
            }
            continue;
        }

        if (i > 1) {
            if (buf[i] != F) {
                i++;
                continue;
            } else {
                if (i == 4) {
                    unsigned char bcc_set = buf[1] ^ buf[2];
                    if (bcc_set == buf[3]) {
                        if (buf[2] != C_SET) {
                            i = 0;
                            continue;
                        }
                        STOP = TRUE;
                        // la momentul acesta stiu ca am primit un SET corect,
                        // deci trimit UA si returnez 1

                        // trimitere UA
                        unsigned char bcc_ua = A_UA ^ C_UA;
                        unsigned char ua[5] = {F, A_UA, C_UA, bcc_ua, F};

                        int bytes = write(fd, ua, 5);
                        if (bytes != 5) {
                            perror(
                                "UAFRAME (5 bytes) not sent in "
                                "llopen_rx()\n");
                            exit(-1);
                        }
                        return 1;
                    } else {
                        i = 0;
                        continue;
                    }
                } else {
                    i = 0;
                    continue;
                }
            }
        }
    }

    printf("llopen_rx() failed\n");
    return 0;
}

////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llopen(LinkLayer connectionParameters) {
    // TODO

    int fd = open(connectionParameters.serialPort, O_RDWR | O_NOCTTY);

    if (fd < 0) {
        perror("Connection FD could not be opened!\n");
        exit(-1);
    }

    /*-----Setarea argumentelor pentru controlul portului serial------*/

    struct termios oldtio;
    struct termios newtio;
    // Save current port settings
    if (tcgetattr(fd, &oldtio) == -1) {
        perror("tcgetattr");
        exit(-1);
    }
    // Clear struct for new port settings
    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    // Set input mode (non-canonical, no echo,...)
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 1;  // Inter-character timer used
    newtio.c_cc[VMIN] = 0;   // Non-Blocking read
    // VTIME e VMIN should be changed in order to protect with a
    // timeout the reception of the following character(s)
    // Now clean the line and activate the settings for the port
    // tcflush() discards data written to the object referred to
    // by fd but not transmitted, or data received but not read,
    // depending on the value of queue_selector:
    //   TCIFLUSH - flushes data received but not read.
    tcflush(fd, TCIOFLUSH);
    // Set new port settings
    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }
    printf("New termios structure set\n");
    /*----------------------------------------------------------------*/

    if (connectionParameters.role == LlRx) {
        if (llopen_rx(connectionParameters, fd) > 0) {
            return fd;
        }

    } else if (connectionParameters.role == LlTx) {
        if (llopen_tx(connectionParameters, fd) > 0) {
            return fd;
        }

    } else {
        perror("Invalid Role\n");
        exit(-1);
    }

    return -1;
}

////////////////////////////////////////////////
// LLWRITE
////////////////////////////////////////////////
int llwrite(int connection_fd, const unsigned char* buf, int bufSize,
            LinkLayer link_struct, int color) {
    unsigned char final_buf[SEND_SIZE] = {0};
    final_buf[0] = F;
    final_buf[1] = A_WRITE;
    if (color == 0) final_buf[2] = C_WHITE;
    if (color == 1) final_buf[2] = C_BLACK;
    final_buf[3] = final_buf[1] ^ final_buf[2];  // BCC1

    unsigned char bcc2 = buf[0];
    final_buf[4] = buf[0];

    int i = 1;
    for (i = 1; i < bufSize; i++) {
        final_buf[i + 4] = buf[i];
        bcc2 = bcc2 ^ buf[i];
    }
    final_buf[i + 4] = bcc2;
    final_buf[i + 5] = F;
    int final_bufSize = bufSize + 6;

    int new_final_bufSize = stuffing(final_buf, final_bufSize);

    //-----implementation of TIMEOUT and RETRANSIMISSION --------

    alarmCount = 0;
    STOP = FALSE;
    alarmEnabled = FALSE;
    (void)signal(SIGALRM, alarmHandler);

resend_frame:

    while (alarmCount <= link_struct.nRetransmissions) {
        if (alarmEnabled == FALSE) {
            int bytes = write(connection_fd, final_buf, new_final_bufSize);

            // printf("Trimit : ");
            // for (int i = 0; i < new_final_bufSize; i++) {
            //     printf("%x ", final_buf[i]);
            // }
            // printf("\n");

            if (bytes == -1) {
                perror("Write error in llwrite()\n");
                exit(-1);
            }

            printf("Frame sent!\n");
            alarm(link_struct.timeout);
            alarmEnabled = TRUE;
            STOP = FALSE;

            // Am trimis data_frame-ul si astept RR sau REJ

            unsigned char recv_buf[512] = {0};
            int i = 0;

            while (STOP == FALSE) {
                int bytes_read = read(connection_fd, recv_buf + i, 1);
                if (bytes_read > 1) {
                    perror("Incorrect read() in llwrite()\n");
                    exit(-1);
                }
                // printf("buf[i] = %x si i=%d\n", recv_buf[i], i);

                switch (i) {
                    case 0:
                        if (recv_buf[i] == F) {
                            i++;
                            continue;
                        } else {
                            continue;
                        }
                        break;

                    case 1:
                        if (recv_buf[i] == F) {
                            continue;
                        } else {
                            i++;
                            continue;
                        }
                        break;

                    default:
                        if (recv_buf[i] != F) {
                            i++;
                            continue;
                        } else {
                            if (i == 4) {
                                unsigned char bcc1 = recv_buf[1] ^ recv_buf[2];
                                if (bcc1 == recv_buf[3]) {
                                    if ((recv_buf[2] == C_REJ_0 &&
                                         color == 0) ||
                                        (recv_buf[2] == C_REJ_1 &&
                                         color == 1)) {
                                        // trebuie sa facem resend!
                                        tcflush(connection_fd, TCIOFLUSH);
                                        printf(
                                            "Received REJ. Need to resend "
                                            "frame!\n\n");
                                        goto resend_frame;
                                    }
                                    if ((recv_buf[2] == C_RR_1 && color == 0) ||
                                        (recv_buf[2] == C_RR_0 && color == 1)) {
                                        // totul bine!
                                        printf(
                                            "Frame acknowledged succesfully!\n\n");
                                        return bytes;
                                    }
                                    i = 0;
                                    continue;
                                } else {
                                    i = 0;
                                    continue;
                                }
                            } else {
                                i = 0;
                                continue;
                            }
                        }
                        break;
                }
            }
        }
    }
    printf("Did not receive REJ or RR\n\n");
    return 0;
}

////////////////////////////////////////////////
// LLREAD
////////////////////////////////////////////////
int llread(int connection_fd, unsigned char* packet, int expected_color) {
    // receiving the stuffed frame byte by byte

    unsigned char buf[BUF_SIZE] = {0};
    int i = 0;
waiting:
    i = 0;

    STOP = FALSE;
    while (STOP == FALSE) {
        int bytes = read(connection_fd, buf + i, 1);
        if (bytes > 1) {
            perror("Invalid read in llread()\n");
            exit(-1);
        }
        // printf("buf[i] = %x si i=%d\n", buf[i], i );
        //  receiving the 1st flag
        switch (i) {
            case 0:
                if (buf[i] != F)
                    continue;
                else {
                    i++;
                    continue;
                }
                break;
            case 1:
                if (buf[i] == A_WRITE) {
                    i++;
                    continue;
                }
                if (buf[i] == F) {
                    continue;
                }
                i = 0;
                continue;
                break;
            default:
                if (buf[i] == F) {
                    STOP = TRUE;
                    continue;
                }
                i++;
                continue;
                break;
        }
    }

    // destuffing the received frame
    int buf_size = i + 1;
    int new_buf_size = destuffing(buf, buf_size);
    int flag = check_received_frame(buf, new_buf_size, expected_color);

    // printf("Primesc : ");
    // for (int i = 0; i < new_buf_size; i++) {
    //     printf("%x ", buf[i]);
    // }
    // printf("\n");
    if (flag == 3) {
        unsigned char bcc_ua = A_UA ^ C_UA;
        unsigned char ua[5] = {F, A_UA, C_UA, bcc_ua, F};

        int bytes = write(connection_fd, ua, 5);
        if (bytes != 5) {
            perror(
                "UAFRAME (5 bytes) not sent in "
                "llopen_rx()\n");
            exit(-1);
        }
        printf("Additional UA required\n");
        return 1;
    }
    if (flag == 0)  // I received the expected packet but it is malformed
    {
        printf("Frame NOT OK!\n");
        send_rej(expected_color, connection_fd);
        goto waiting;

    } else if (flag == 2)  // I received a duplicate of the last packet
    {
        printf("Duplicate frame!\n");
        send_rr(expected_color, connection_fd);
        goto waiting;

    } else {  // Everything was ok!
        printf("Frame OK!\n");
        send_rr((expected_color + 1) % 2, connection_fd);

        memcpy(packet, buf + 4, new_buf_size - 4 - 2);
        return new_buf_size - 6;
    }

    return 0;
}

////////////////////////////////////////////////
// LLCLOSE_TRANSMITOR
////////////////////////////////////////////////
int llclose_tx(LinkLayer transmitter, int fd) {
    alarmCount = 0;
    STOP = FALSE;
    alarmEnabled = FALSE;
    (void)signal(SIGALRM, alarmHandler);

    while (alarmCount <= transmitter.nRetransmissions) {
        STOP = FALSE;

        if (alarmEnabled == FALSE) {
            unsigned char bcc_disc = A_DISC_TX ^ C_DISC;
            unsigned char disc[5] = {F, A_DISC_TX, C_DISC, bcc_disc, F};

            int bytes = write(fd, disc, 5);
            if (bytes != 5) {
                perror("DISCFRAME (5 bytes) not sent in llclose_tx()\n");
                exit(-1);
            }
            alarm(transmitter.timeout);
            alarmEnabled = TRUE;

            unsigned char buf[BUF_SIZE] = {0};

            int i = 0;
            while (STOP == FALSE) {
                int bytes = read(fd, buf + i, 1);
                if (bytes > 1) {
                    perror("Invalid reading!\n");
                    continue;
                }

                if (i == 0) {
                    if (buf[i] == F) {
                        i++;
                    }
                    continue;
                }

                if (i == 1) {
                    if (buf[i] == A_DISC_RX) {
                        i++;
                        continue;
                    }
                    if (buf[i] == F) {
                        continue;
                    }
                    i = 0;
                    continue;
                }

                if (i > 1) {
                    if (buf[i] != F) {
                        i++;
                        continue;
                    } else {
                        if (i == 4) {
                            unsigned char bcc_disc_rx = buf[1] ^ buf[2];
                            if (bcc_disc_rx == buf[3]) {
                                if (buf[2] != C_DISC) {
                                    i = 0;
                                    continue;
                                }
                                printf("DISC frame received!\n");
                                STOP = TRUE;
                                goto exit_loops;

                            } else {
                                i = 0;
                                continue;
                            }
                        } else {
                            i = 0;
                            continue;
                        }
                    }
                }
            }
            // for (int j = 0; j < 5; j++) {
            //     printf("%x\n", buf[j]);
            // }
        }
    }

    unsigned char bcc_ua = 0x00;
    unsigned char ua[5] = {0};

exit_loops:

    bcc_ua = A_DISC_RX ^ C_UA;
    ua[0] = F;
    ua[1] = A_DISC_RX;
    ua[2] = C_UA;
    ua[3] = bcc_ua;
    ua[4] = F;

    int bytes = write(fd, ua, 5);
    if (bytes != 5) {
        perror(
            "UAFRAME (5 bytes) not sent in "
            "llclose_tx()\n");
        exit(-1);
    }
    //printf("llclose_tx() succesful\n");

    return 1;
}

////////////////////////////////////////////////
// LLCLOSE_RECEIVER
////////////////////////////////////////////////
int llclose_rx(LinkLayer receiver, int fd) {
    unsigned char buf[BUF_SIZE] = {0};

    STOP = FALSE;

    int i = 0;
    while (STOP == FALSE) {
        int bytes = read(fd, buf + i, 1);
        if (bytes > 1) {
            perror("Invalid reading!\n");
            continue;
        }
        //printf("%x, i=%d\n", buf[i], i);

        if (i == 0) {
            if (buf[i] == F) {
                i++;
            }
            continue;
        }

        if (i == 1) {
            if (buf[i] == A_DISC_TX) {
                i++;
                continue;
            }
            if (buf[i] == F) {
                continue;
            }
            i = 0;
            continue;
        }

        if (i > 1) {
            if (buf[i] != F) {
                i++;
                continue;
            } else {
                if (i == 4) {
                    unsigned char bcc_disc = buf[1] ^ buf[2];
                    if (bcc_disc == buf[3]) {
                        if (buf[2] != C_DISC) {
                            i = 0;
                            continue;
                        }

                        STOP = TRUE;

                        unsigned char bcc_disc = A_DISC_RX ^ C_DISC;
                        unsigned char disc[5] = {F, A_DISC_RX, C_DISC, bcc_disc,
                                                 F};

                        int bytes = write(fd, disc, 5);
                        if (bytes != 5) {
                            perror(
                                "DISCFRAME (5 bytes) not sent in "
                                "llclose_rx()\n");
                            exit(-1);
                        }

                    } else {
                        i = 0;
                        continue;
                    }
                } else {
                    i = 0;
                    continue;
                }
            }
        }
    }

    i = 0;
    STOP = FALSE;
    while (STOP == FALSE) {
        int bytes = read(fd, buf + i, 1);
        if (bytes > 1) {
            perror("Invalid reading!\n");
            continue;
        }

        if (i == 0) {
            if (buf[i] == F) {
                i++;
            }
            continue;
        }

        if (i == 1) {
            if (buf[i] == A_DISC_RX) {
                i++;
                continue;
            }
            if (buf[i] == F) {
                continue;
            }
            i = 0;
            continue;
        }

        if (i > 1) {
            if (buf[i] != F) {
                i++;
                continue;
            } else {
                if (i == 4) {
                    unsigned char bcc_ua = buf[1] ^ buf[2];
                    if (bcc_ua == buf[3]) {
                        if (buf[2] != C_UA) {
                            i = 0;
                            continue;
                        }
                        STOP = TRUE;
                        return 1;
                    } else {
                        i = 0;
                        continue;
                    }
                } else {
                    i = 0;
                    continue;
                }
            }
        }
    }
    printf("llclose_rx() unsuccesful\n");
    return 0;
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llclose(int fd, LinkLayer connectionParameters, int showStatistics) {
    // TODO
    if (connectionParameters.role == LlRx) {
        if (llclose_rx(connectionParameters, fd) > 0) {
            int ok = close(fd);
            if (ok == 0) return 1;
            return ok;
        }

    } else if (connectionParameters.role == LlTx) {
        if (llclose_tx(connectionParameters, fd) > 0) {
            int ok = close(fd);
            if (ok == 0) return 1;
            return ok;
        }
    }

    return -1;
}
