/*   (c) 2015 befinitiv
 *   modified 2018 by Wolfgang Christl (integration into DroneBridge https://github.com/DroneBridge)
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include <stdbool.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <zconf.h>
#include <memory.h>
#include <errno.h>
#include <signal.h>
#include "fec.h"


#include "video_lib.h"
#include "radiotap.h"
#include "../common/shared_memory.h"
#include "../common/db_raw_receive.h"
#include "../common/db_get_ip.h"
#include "../common/ccolors.h"

#define MAX_PACKET_LENGTH 4192
#define MAX_USER_PACKET_LENGTH 1450
#define MAX_DATA_OR_FEC_PACKETS_PER_BLOCK 32

#define DEBUG 0
#define debug_print(fmt, ...) \
            do { if (DEBUG) fprintf(stderr, fmt, __VA_ARGS__); } while (0)

int num_interfaces = 0;
uint8_t comm_id, num_data_block, num_fec_block;
uint8_t lr_buffer[DATA_UNI_LENGTH] = {0};
bool pass_through, keeprunning = true, udp_enabled = true;
int param_block_buffers = 1;
int pack_size = MAX_USER_PACKET_LENGTH;
db_video_rx_t *rx_status = NULL;
int max_block_num = -1, udp_socket, shID;
struct sockaddr_in client_video_addr;

char adapters[DB_MAX_ADAPTERS][IFNAMSIZ];

typedef struct {
    int selectable_fd;
    int n80211HeaderLength;
} monitor_interface_t;


void int_handler(int dummy){
    keeprunning = false;
}


void init_outputs(){
    int app_port_video = APP_PORT_VIDEO;
    if (pass_through) app_port_video = APP_PORT_VIDEO_FEC;
    if (udp_enabled){
        udp_socket = socket (AF_INET, SOCK_DGRAM, 0);
        client_video_addr.sin_family = AF_INET;
        client_video_addr.sin_addr.s_addr = inet_addr("192.168.2.2");
        client_video_addr.sin_port = htons(app_port_video);
    }
}

void publish_data(uint8_t *data, size_t message_length, bool fec_decoded){
    if (udp_enabled){
        client_video_addr.sin_addr.s_addr = inet_addr(get_ip_from_ipchecker(shID));
        sendto (udp_socket, data, message_length, 0, (struct sockaddr *) &client_video_addr,
                sizeof (client_video_addr));
    }
    if (fec_decoded) // only output decoded fec packets to stdout so that video player can read data stream directly
        if (write(STDOUT_FILENO, data, message_length) < message_length)
            printf(RED "Not all data written to stdout\n" RESET);
    // TODO: setup a TCP server and send to connected clients
}

void block_buffer_list_reset(block_buffer_t *block_buffer_list, size_t block_buffer_list_len, int block_buffer_len) {
    int i;
    block_buffer_t *rb = block_buffer_list;

    for(i=0; i<block_buffer_list_len; ++i) {
        rb->block_num = -1;

        int j;
        packet_buffer_t *p = rb->packet_buffer_list;
        for(j=0; j<num_data_block+num_fec_block; ++j) {
            p->valid = 0;
            p->crc_correct = 0;
            p->len = 0;
            p++;
        }

        rb++;
    }
}

void process_payload(uint8_t *data, size_t data_len, int crc_correct, block_buffer_t *block_buffer_list)
{
    wifi_packet_header_t *wph;
    int block_num;
    int packet_num;
    int i;

    wph = (wifi_packet_header_t*)data;
    data += sizeof(wifi_packet_header_t);
    data_len -= sizeof(wifi_packet_header_t);

    block_num = wph->sequence_number / (num_data_block+num_fec_block);//if aram_data_packets_per_block+num_fec_block would be limited to powers of two, this could be replaced by a logical AND operation

    //we have received a block number that exceeds the currently seen ones -> we need to make room for this new block
    //or we have received a block_num that is several times smaller than the current window of buffers -> this indicated that either the window is too small or that the transmitter has been restarted
    int tx_restart = (block_num + 128*param_block_buffers < max_block_num);
    if((block_num > max_block_num || tx_restart) && crc_correct) {
        if(tx_restart) {
            rx_status->tx_restart_cnt++;

            fprintf(stderr, "TX RESTART: Detected blk %x that lies outside of the current retr block buffer window (max_block_num = %x) (if there was no tx restart, increase window size via -d)\n", block_num, max_block_num);


            block_buffer_list_reset(block_buffer_list, param_block_buffers, num_data_block + num_fec_block);
        }

        //first, find the minimum block num in the buffers list. this will be the block that we replace
        int min_block_num = INT_MAX;
        int min_block_num_idx;
        for(i=0; i<param_block_buffers; ++i) {
            if(block_buffer_list[i].block_num < min_block_num) {
                min_block_num = block_buffer_list[i].block_num;
                min_block_num_idx = i;
            }
        }
        //debug_print("removing block %x at index %i for block %x\n", min_block_num, min_block_num_idx, block_num);

        packet_buffer_t *packet_buffer_list = block_buffer_list[min_block_num_idx].packet_buffer_list;
        int last_block_num = block_buffer_list[min_block_num_idx].block_num;

        if(last_block_num != -1) {
            rx_status->received_block_cnt++;

            //we have both pointers to the packet buffers (to get information about crc and vadility) and raw data pointers for fec_decode
            packet_buffer_t *data_pkgs[MAX_DATA_OR_FEC_PACKETS_PER_BLOCK];
            packet_buffer_t *fec_pkgs[MAX_DATA_OR_FEC_PACKETS_PER_BLOCK];
            uint8_t *data_blocks[MAX_DATA_OR_FEC_PACKETS_PER_BLOCK];
            uint8_t *fec_blocks[MAX_DATA_OR_FEC_PACKETS_PER_BLOCK];
            int datas_missing = 0, datas_corrupt = 0, fecs_missing = 0, fecs_corrupt = 0;
            int di = 0, fi = 0;

            //first, split the received packets into DATA a FEC packets and count the damaged packets
            i = 0;
            while(di < num_data_block || fi < num_fec_block) {
                if(di < num_data_block) {
                    data_pkgs[di] = packet_buffer_list + i++;
                    data_blocks[di] = data_pkgs[di]->data;
                    if(!data_pkgs[di]->valid)
                        datas_missing++;
                    if(data_pkgs[di]->valid && !data_pkgs[di]->crc_correct)
                        datas_corrupt++;
                    di++;
                }

                if(fi < num_fec_block) {
                    fec_pkgs[fi] = packet_buffer_list + i++;
                    if(!fec_pkgs[fi]->valid)
                        fecs_missing++;

                    if(fec_pkgs[fi]->valid && !fec_pkgs[fi]->crc_correct)
                        fecs_corrupt++;

                    fi++;
                }
            }

            const int good_fecs_c = num_fec_block - fecs_missing - fecs_corrupt;
            const int datas_missing_c = datas_missing;
            const int datas_corrupt_c = datas_corrupt;
            const int fecs_missing_c = fecs_missing;
            const int fecs_corrupt_c = fecs_corrupt;

            int good_fecs = good_fecs_c;
            //the following three fields are infos for fec_decode
            unsigned int fec_block_nos[MAX_DATA_OR_FEC_PACKETS_PER_BLOCK];
            unsigned int erased_blocks[MAX_DATA_OR_FEC_PACKETS_PER_BLOCK];
            unsigned int nr_fec_blocks = 0;

#if DEBUG
            if(datas_missing_c + datas_corrupt_c > good_fecs_c) {
                int x;

                for(x=0;x<num_data_block; ++x) {
                    if(data_pkgs[x]->valid) {
                        if(data_pkgs[x]->crc_correct)
                            fprintf(stderr, "v");
                        else
                            fprintf(stderr, "c");
                    }
                    else
                        fprintf(stderr, "m");
                }

                fprintf(stderr, " ");

                for(x=0;x<num_fec_block; ++x) {
                    if(fec_pkgs[x]->valid) {
                        if(fec_pkgs[x]->crc_correct)
                            fprintf(stderr, "v");
                        else
                            fprintf(stderr, "c");
                    }
                    else
                        fprintf(stderr, "m");
                }

                fprintf(stderr, "\n");
            }
#endif

            fi = 0;
            di = 0;

            //look for missing DATA and replace them with good FECs
            while(di < num_data_block && fi < num_fec_block) {
                //if this data is fine we go to the next
                if(data_pkgs[di]->valid && data_pkgs[di]->crc_correct) {
                    di++;
                    continue;
                }
                //if this DATA is corrupt and there are less good fecs than missing datas we cannot do anything for this data
                if(data_pkgs[di]->valid && !data_pkgs[di]->crc_correct && good_fecs <= datas_missing) {
                    di++;
                    continue;
                }
                //if this FEC is not received we go on to the next
                if(!fec_pkgs[fi]->valid) {
                    fi++;
                    continue;
                }
                //if this FEC is corrupted and there are more lost packages than good fecs we should replace this DATA even with this corrupted FEC
                if(!fec_pkgs[fi]->crc_correct && datas_missing > good_fecs) {
                    fi++;
                    continue;
                }

                if(!data_pkgs[di]->valid)
                    datas_missing--;
                else if(!data_pkgs[di]->crc_correct)
                    datas_corrupt--;

                if(fec_pkgs[fi]->crc_correct)
                    good_fecs--;

                //at this point, data is invalid and fec is good -> replace data with fec
                erased_blocks[nr_fec_blocks] = di;
                fec_block_nos[nr_fec_blocks] = fi;
                fec_blocks[nr_fec_blocks] = fec_pkgs[fi]->data;
                di++;
                fi++;
                nr_fec_blocks++;
            }


            int reconstruction_failed = datas_missing_c + datas_corrupt_c > good_fecs_c;

            if(reconstruction_failed) {
                //we did not have enough FEC packets to repair this block
                rx_status->damaged_block_cnt++;
                fprintf(stderr, "Could not fully reconstruct block %x! Damage rate: %f (%d / %d blocks)\n",
                        last_block_num, 1.0 * rx_status->damaged_block_cnt / rx_status->received_block_cnt,
                        rx_status->damaged_block_cnt, rx_status->received_block_cnt);
                debug_print("Data mis: %d\tData corr: %d\tFEC mis: %d\tFEC corr: %d\n", datas_missing_c,
                        datas_corrupt_c, fecs_missing_c, fecs_corrupt_c);
            }

            //decode data and write it to STDOUT
            fec_decode((unsigned int) pack_size, data_blocks, num_data_block, fec_blocks, fec_block_nos, erased_blocks, nr_fec_blocks);
            for(i=0; i<num_data_block; ++i) {
                payload_header_t *ph = (payload_header_t*)data_blocks[i];

                if(!reconstruction_failed || data_pkgs[i]->valid) {
                    //if reconstruction did fail, the data_length value is undefined. better limit it to some sensible value
                    if(ph->data_length > pack_size)
                        ph->data_length = pack_size;

                    publish_data(data_blocks[i] + sizeof(payload_header_t), ph->data_length, true);
                }
            }

            //reset buffers
            for(i=0; i<num_data_block + num_fec_block; ++i) {
                packet_buffer_t *p = packet_buffer_list + i;
                p->valid = 0;
                p->crc_correct = 0;
                p->len = 0;
            }
        }

        block_buffer_list[min_block_num_idx].block_num = block_num;
        max_block_num = block_num;
    }

    //find the buffer into which we have to write this packet
    block_buffer_t *rbb = block_buffer_list;
    for(i=0; i<param_block_buffers; ++i) {
        if(rbb->block_num == block_num) {
            break;
        }
        rbb++;
    }

    //check if we have actually found the corresponding block. this could not be the case due to a corrupt packet
    if(i != param_block_buffers) {
        packet_buffer_t *packet_buffer_list = rbb->packet_buffer_list;
        packet_num = wph->sequence_number % (num_data_block+num_fec_block); //if retr_block_size would be limited to powers of two, this could be replace by a locical and operation

        //only overwrite packets where the checksum is not yet correct. otherwise the packets are already received correctly
        if(packet_buffer_list[packet_num].crc_correct == 0) {
            memcpy(packet_buffer_list[packet_num].data, data, data_len);
            packet_buffer_list[packet_num].len = data_len;
            packet_buffer_list[packet_num].valid = 1;
            packet_buffer_list[packet_num].crc_correct = crc_correct;
        }
    }

}


void process_packet(monitor_interface_t *interface, block_buffer_t *block_buffer_list, int adapter_no) {
    struct ieee80211_radiotap_iterator rti;
    PENUMBRA_RADIOTAP_DATA prd;
    uint8_t payload_buffer[MAX_PACKET_LENGTH];
    int radiotap_length = 0;
    size_t message_length = 0;

    // receive


    ssize_t l = recv(interface->selectable_fd, lr_buffer, DATA_UNI_LENGTH, 0); int err = errno;
    if (l > 0){
        radiotap_length = lr_buffer[2] | (lr_buffer[3] << 8);
        message_length = lr_buffer[radiotap_length+7] | (lr_buffer[radiotap_length+8] << 8); // DB_v2
        memcpy(payload_buffer, lr_buffer+(radiotap_length + DB_RAW_V2_HEADER_LENGTH), message_length);
        if (pass_through){
            // Do not decode using FEC - pure UDP pass through, decoding of FEC must happen on following applications
            // TODO: Implement custom protocol in case of pass_through that tells the receiver about the adapter that it was received on
            publish_data(payload_buffer, message_length, false);
        }
        if (ieee80211_radiotap_iterator_init(&rti, (struct ieee80211_radiotap_header *)payload_buffer, radiotap_length) < 0)
            return;
        while ((ieee80211_radiotap_iterator_next(&rti)) == 0) {
            switch (rti.this_arg_index) {
                case IEEE80211_RADIOTAP_RATE:
                    prd.m_nRate = (*rti.this_arg);
                    break;
                case IEEE80211_RADIOTAP_CHANNEL:
                    prd.m_nChannel =
                            le16_to_cpu(*((u16 *)rti.this_arg));
                    prd.m_nChannelFlags =
                            le16_to_cpu(*((u16 *)(rti.this_arg + 2)));
                    break;
                case IEEE80211_RADIOTAP_ANTENNA:
                    prd.m_nAntenna = (*rti.this_arg) + 1;
                    break;
                case IEEE80211_RADIOTAP_FLAGS:
                    prd.m_nRadiotapFlags = *rti.this_arg;
                    break;
                case IEEE80211_RADIOTAP_DBM_ANTSIGNAL:
                    rx_status->adapter[adapter_no].current_signal_dbm = (int8_t)(*rti.this_arg);
                    break;
                default:
                    break;
            }
        }
        int checksum_correct = (prd.m_nRadiotapFlags & 0x40) == 0;

        if(!checksum_correct)
            rx_status->adapter[adapter_no].wrong_crc_cnt++;
        rx_status->adapter[adapter_no].received_packet_cnt++;
        if(rx_status->adapter[adapter_no].received_packet_cnt % 1024 == 0) {
            fprintf(stderr, "Signal (card %d): %ddBm\n", adapter_no, rx_status->adapter[adapter_no].current_signal_dbm);
        }

        rx_status->last_update = time(NULL);
        process_payload(payload_buffer, message_length, checksum_correct, block_buffer_list);
    } else {
        printf(RED "DB_VIDEO_GND: Received an error: %s\n" RESET, strerror(err));
    }
}

void process_command_line_args(int argc, char *argv[]){
    num_interfaces = 0;
    num_data_block = 8, num_fec_block = 4, pack_size = 1024;
    int c;
    while ((c = getopt (argc, argv, "n:c:b:r:f:p:")) != -1) {
        switch (c) {
            case 'n':
                strncpy(adapters[num_interfaces], optarg, IFNAMSIZ);
                num_interfaces++;
                break;
            case 'c':
                comm_id = (uint8_t) strtol(optarg, NULL, 10);
                break;
            case 'd':
                num_data_block = (uint8_t) strtol(optarg, NULL, 10);
                break;
            case 'r':
                num_fec_block = (uint8_t) strtol(optarg, NULL, 10);
                break;
            case 'f':
                pack_size = (uint8_t) strtol(optarg, NULL, 10);
                break;
            case 'p':
                if (*optarg == 'Y')
                    pass_through = true; // encoded FEC packets pass through via UDP
                break;
            default:
                printf("Based of Wifibroadcast by befinitiv, based on packetspammer by Andy Green.  Licensed under GPL2\n"
                       "This tool takes a data stream via the DroneBridge long range video port and outputs it via stdout, "
                       "UDP or TCP"
                       "\nIt uses the Reed-Solomon FEC code to repair lost or damaged packets."
                       "\n\n\t-n Name of a network interface that should be used to receive the stream. Must be in monitor "
                       "mode. Multiple interfaces supported by calling this option multiple times (-n inter1 -n inter2 -n interx)"
                       "\n\t-c <communication id> Choose a number from 0-255. Same on ground station and UAV!."
                       "\n\t-d Number of data packets in a block (default 8). Needs to match with tx."
                       "\n\t-r Number of FEC packets per block (default 4). Needs to match with tx."
                       "\n\t-f Bytes per packet (default %d. max %d). This is also the FEC "
                       "block size. Needs to match with tx."
                       "\n\t <Y|N> to enable/disable pass through of encoded FEC packets via UDP to port: %i"
                        , 1024, MAX_USER_PACKET_LENGTH, APP_PORT_VIDEO_FEC);
                abort();
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, int_handler);
    setpriority(PRIO_PROCESS, 0, -10);
    monitor_interface_t interfaces[MAX_PENUMBRA_INTERFACES];
    int num_interfaces = 0;
    int i, max_sd = 0;
    block_buffer_t *block_buffer_list;

    process_command_line_args(argc, argv);

    if(pack_size > MAX_USER_PACKET_LENGTH) {
        printf("Packet length is limited to %d bytes (you requested %d bytes)\n", MAX_USER_PACKET_LENGTH, pack_size);
        abort();
    }

    fec_init();
    shID = init_shared_memory_ip();
    init_outputs();

    for (int j = 0; j < num_interfaces; ++j) {
        interfaces[j].selectable_fd = open_receive_socket(adapters[j], 'm', comm_id, DB_DIREC_GROUND, DB_PORT_VIDEO);
    }

    //block buffers contain both the block_num as well as packet buffers for a block.
    block_buffer_list = malloc(sizeof(block_buffer_t) * param_block_buffers);
    for(i=0; i<param_block_buffers; ++i)
    {
        block_buffer_list[i].block_num = -1;
        block_buffer_list[i].packet_buffer_list = lib_alloc_packet_buffer_list(num_data_block+num_fec_block, MAX_PACKET_LENGTH);
    }


    rx_status = db_video_rx_memory_open();
    rx_status->wifi_adapter_cnt = (uint32_t) num_interfaces;

    while(keeprunning) {
        fd_set readset;
        struct timeval to;
        to.tv_sec = 0;
        to.tv_usec = 1e5;

        FD_ZERO(&readset);
        for(i = 0; i < num_interfaces; ++i) {
            FD_SET(interfaces[i].selectable_fd, &readset);
            if (interfaces[i].selectable_fd > max_sd) max_sd = interfaces[i].selectable_fd;
        }

        int select_return = select(max_sd, &readset, NULL, NULL, &to);

        for(i=0; i<num_interfaces; ++i) {
            if(select_return == 0)
                break;
            if(FD_ISSET(interfaces[i].selectable_fd, &readset)) {
                process_packet(interfaces + i, block_buffer_list, i);
            }
        }

    }

    for(int g = 0; i < num_interfaces; ++i){
        close(interfaces[g].selectable_fd);
    }
    if(udp_enabled) close(udp_socket);
    return (0);
}