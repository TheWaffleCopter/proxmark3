//-----------------------------------------------------------------------------
// Borrowed initially from https://github.com/Proxmark/proxmark3/pull/167/files
// Copyright (C) 2016 Oguzhan Cicek, Hendrik Schwartke, Ralf Spenneberg
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Hitag S emulation (preliminary test version)
//-----------------------------------------------------------------------------

#include "hitagS.h"
#include "hitag_common.h"

#include "proxmark3_arm.h"
#include "cmd.h"
#include "BigBuf.h"
#include "fpgaloader.h"
#include "ticks.h"
#include "dbprint.h"
#include "util.h"
#include "string.h"
#include "commonutil.h"
#include "hitag2/hitag2_crypto.h"
#include "lfadc.h"
#include "crc.h"
#include "protocols.h"
#include "appmain.h"    // tearoff_hook()

static struct hitagS_tag tag = {
    .data.pages = {
        // Plain mode:               | Authentication mode:
        [0] = {0x5F, 0xC2, 0x11, 0x84},    // UID                       | UID
        // HITAG S 2048
        [1] = {0xCA, 0x00, 0x00, 0xAA},    // CON0 CON1 CON2 Reserved   | CON0 CON1 CON2 PWDH0
        [2] = {0x48, 0x54, 0x4F, 0x4E},    // Data                      | PWDL0 PWDL1 KEYH0 KEYH1
        [3] = {0x4D, 0x49, 0x4B, 0x52},    // Data                      | KEYL0 KEYL1 KEYL2 KEYL3
        [4] = {0xFF, 0x80, 0x00, 0x00},    // Data
        [5] = {0x00, 0x00, 0x00, 0x00},    // Data
        [6] = {0x00, 0x00, 0x00, 0x00},    // Data
        [7] = {0x57, 0x5F, 0x4F, 0x48},    // Data
        // up to index 63 for HITAG S2048 public data
    },
};
static uint8_t page_to_be_written = 0;
static int block_data_left = 0;
static bool enable_page_tearoff = false;

static uint8_t protocol_mode = HITAGS_UID_REQ_ADV1;
static MOD m = AC2K;                                // used modulation
static uint32_t reader_selected_uid;
static int rotate_uid = 0;
static int sof_bits;                                // number of start-of-frame bits
static uint8_t pwdh0, pwdl0, pwdl1;                 // password bytes
static uint8_t rnd[] = {0x85, 0x44, 0x12, 0x74};    // random number

//#define SENDBIT_TEST

/* array index 3 2 1 0 // bytes in sim.bin file are 0 1 2 3
UID is 0 1 2 3 // tag.data.s.uid_le is 3210
datasheet HitagS_V11.pdf bytes in tables printed 3 2 1 0

#db# UID: 5F C2 11 84
#db# conf0: C9 conf1: 00 conf2: 00
                3  2  1  0
#db# Page[ 0]: 84 11 C2 5F uid
#db# Page[ 1]: AA 00 00 C9 conf, HITAG S 256
#db# Page[ 2]: 4E 4F 54 48
#db# Page[ 3]: 52 4B 49 4D
#db# Page[ 4]: 00 00 00 00
#db# Page[ 5]: 00 00 00 00
#db# Page[ 6]: 00 00 00 00
#db# Page[ 7]: 4B 4F 5F 57 */

#define ht2bs_4a(a,b,c,d)   (~(((a|b)&c)^(a|d)^b))
#define ht2bs_4b(a,b,c,d)   (~(((d|c)&(a^b))^(d|a|b)))
#define ht2bs_5c(a,b,c,d,e) (~((((((c^e)|d)&a)^b)&(c^b))^(((d^e)|a)&((d^b)|c))))

static void update_tag_max_page(void) {
    //check which memorysize this tag has
    if (tag.data.s.config.MEMT == 0x00) {
        tag.max_page = 32 / (HITAGS_PAGE_SIZE * 8) - 1;
    } else if (tag.data.s.config.MEMT == 0x1) {
        tag.max_page = 256 / (HITAGS_PAGE_SIZE * 8) - 1;
    } else if (tag.data.s.config.MEMT == 0x2) {
        tag.max_page = 2048 / (HITAGS_PAGE_SIZE * 8) - 1;
    } else {
        tag.max_page = HITAGS_MAX_PAGES - 1;
    }
}

/*
 * to check if the right uid was selected
 */
static int check_select(const uint8_t *rx, uint32_t uid) {

    // global var?
    concatbits((uint8_t *)&reader_selected_uid, 0, rx, 5, 32, false);
    reader_selected_uid = BSWAP_32(reader_selected_uid);

    if (reader_selected_uid == uid) {
        return 1;
    }

    return 0;
}

static void hts_set_frame_modulation(uint8_t mode, bool ac_seq) {
    switch (mode) {
        case HITAGS_UID_REQ_STD: {
            sof_bits = 1;
            if (ac_seq)
                m = AC2K;
            else
                m = MC4K;
            break;
        }
        case HITAGS_UID_REQ_ADV1:
        case HITAGS_UID_REQ_ADV2: {
            if (ac_seq) {
                sof_bits = 3;
                m = AC2K;
            } else {
                sof_bits = 6;
                m = MC4K;
            }
            break;
        }
        case HITAGS_UID_REQ_FADV: {
            if (ac_seq) {
                sof_bits = 3;
                m = AC4K;
            } else {
                sof_bits = 6;
                m = MC8K;
            }
            break;
        }
    }
}

/*
 * handles all commands from a reader
 */
static void hts_handle_reader_command(uint8_t *rx, const size_t rxlen,
                                      uint8_t *tx, size_t *txlen) {
    uint64_t state;
    unsigned char crc;

    // Reset the transmission frame length
    *txlen = 0;
    // Reset the frame modulation
    hts_set_frame_modulation(protocol_mode, false);

    // Try to find out which command was send by selecting on length (in bits)
    switch (rxlen) {
        case 5: {
            //UID request with a selected response protocol mode
            DBG Dbprintf("UID request: length: %i first byte: %02x", rxlen, rx[0]);
            tag.pstate = HT_READY;
            tag.tstate = HT_NO_OP;

            if (rx[0] == HITAGS_UID_REQ_STD) {
                DBG Dbprintf("HT_STANDARD");
            } else if (rx[0] == HITAGS_UID_REQ_ADV1 || rx[0] == HITAGS_UID_REQ_ADV2) {
                DBG Dbprintf("HT_ADVANCED");
            } else if (rx[0] == HITAGS_UID_REQ_FADV) {
                DBG Dbprintf("HT_FAST_ADVANCED");
            }

            protocol_mode = rx[0];
            hts_set_frame_modulation(protocol_mode, true);

            //send uid as a response
            *txlen = 32;
            memcpy(tx, tag.data.pages[HITAGS_UID_PADR], HITAGS_PAGE_SIZE);
            break;
        }
        // case 14 to 44 AC SEQUENCE
        case 45: {
            //select command from reader received
            DBG DbpString("SELECT");

            if ((rx[0] & 0xf8) == HITAGS_SELECT && check_select(rx, BSWAP_32(tag.data.s.uid_le)) == 1) {
                DBG DbpString("SELECT match");

                //if the right tag was selected
                *txlen = 32;

                //send configuration
                memcpy(tx, tag.data.pages[HITAGS_CONFIG_PADR], HITAGS_PAGE_SIZE - 1);

                tx[3] = 0xff;

                if (protocol_mode != HITAGS_UID_REQ_STD) {
                    //add crc8
                    crc = CRC8Hitag1Bits(tx, 32);
                    *txlen += 8;
                    tx[4] = crc;
                }
            }
            break;
        }
        case 64: {
            //challenge message received
            DBG Dbprintf("Challenge for UID: %X", reader_selected_uid);

            rotate_uid++;
            *txlen = 32;
            // init crypt engine
            uint32_t le_rx = MemLeToUint4byte(rx);
            state = ht2_hitag2_init(REV64(tag.data.s.key), REV32(tag.data.s.uid_le), REV32(le_rx));
            DBG Dbhexdump(8, tx, false);

            for (int i = 0; i < 4; i++) {
                ht2_hitag2_byte(&state);
            }

            // store plaintext first
            tx[0] = tag.data.pages[HITAGS_CONFIG_PADR][2];
            tx[1] = tag.data.s.config.pwdh0;
            tx[2] = tag.data.s.pwdl0;
            tx[3] = tag.data.s.pwdl1;

            if (protocol_mode != HITAGS_UID_REQ_STD) {
                // add crc8
                *txlen += 8;
                crc = CRC8Hitag1Bits(tx, 32);
                tx[4] = crc;
            }

            // then xor with keystream
            tx[0] ^= ht2_hitag2_byte(&state);
            tx[1] ^= ht2_hitag2_byte(&state);
            tx[2] ^= ht2_hitag2_byte(&state);
            tx[3] ^= ht2_hitag2_byte(&state);
            if (protocol_mode != HITAGS_UID_REQ_STD) {
                tx[4] ^= ht2_hitag2_byte(&state);
            }

            /*
             * some readers do not allow to authenticate multiple times in a row with the same tag.
             * use this to change the uid between authentications.
             if (rotate_uid % 2 == 0) {
                 tag.data.s.uid_le = 0x44332211;
             } else {
                 tag.data.s.uid_le = 0x88776655;
             }
             */
            break;
        }
        case 40: {
            DBG Dbprintf("WRITE DATA");

            //data received to be written
            if (tag.tstate == HT_WRITING_PAGE_DATA) {
                tag.tstate = HT_NO_OP;
                memcpy(tag.data.pages[page_to_be_written], rx, HITAGS_PAGE_SIZE);
                //send ack
                *txlen = 2;
                tx[0] = 0x40;
                page_to_be_written = 0;

            } else if (tag.tstate == HT_WRITING_BLOCK_DATA) {
                memcpy(tag.data.pages[page_to_be_written], rx, HITAGS_PAGE_SIZE);
                //send ack
                *txlen = 2;
                tx[0] = 0x40;
                page_to_be_written++;
                block_data_left--;

                if (block_data_left == 0) {
                    tag.tstate = HT_NO_OP;
                    page_to_be_written = 0;
                }
            }
            break;
        }
        case 20: {
            //write page, write block, read page or read block command received
            uint8_t page = ((rx[0] & 0x0f) << 4) + ((rx[1] & 0xf0) >> 4);
            // TODO: handle over max_page readonly to 00000000. 82xx mode
            if (page > tag.max_page) {
                *txlen = 0;
                break;
            }

            if ((rx[0] & 0xf0) == HITAGS_READ_PAGE) { //read page
                //send page data
                *txlen = 32;
                memcpy(tx, tag.data.pages[page], HITAGS_PAGE_SIZE);

                if (tag.data.s.config.auth && page == HITAGS_CONFIG_PADR) {
                    tx[3] = 0xFF;
                }

                if (protocol_mode != HITAGS_UID_REQ_STD) {
                    //add crc8
                    crc = CRC8Hitag1Bits(tx, 32);
                    *txlen += 8;
                    tx[4] = crc;
                }

                if (tag.data.s.config.auth && tag.data.s.config.LKP && (page == 2 || page == 3)) {
                    //if reader asks for key or password and the LKP-mark is set do not respond
                    *txlen = 0;
                }

            } else if ((rx[0] & 0xf0) == HITAGS_READ_BLOCK) { //read block
                // TODO: handle auth LKP
                *txlen = (HITAGS_BLOCK_SIZE - (page % 4) * HITAGS_PAGE_SIZE) * 8;

                //send page,...,page+3 data
                memcpy(tx, tag.data.pages[page], *txlen / 8);

                if (protocol_mode != HITAGS_UID_REQ_STD) {
                    //add crc8
                    crc = CRC8Hitag1Bits(tx, *txlen);
                    *txlen += 8;
                    tx[16] = crc;
                }

            } else if ((rx[0] & 0xf0) == HITAGS_WRITE_PAGE) { //write page
                // TODO: handle con2 LCK*
                if ((tag.data.s.config.LCON && page == 1)
                        || (tag.data.s.config.LKP && (page == 2 || page == 3))) {
                    //deny
                    *txlen = 0;
                } else {
                    //allow
                    *txlen = 2;
                    tx[0] = 0x40;
                    page_to_be_written = page;
                    tag.tstate = HT_WRITING_PAGE_DATA;
                }

            } else if ((rx[0] & 0xf0) == HITAGS_WRITE_BLOCK) { //write block
                // TODO: handle LCON con2 LCK*
                if ((tag.data.s.config.LCON && page == 1)
                        || (tag.data.s.config.LKP && (page == 2 || page == 3))) {
                    //deny
                    *txlen = 0;
                } else {
                    //allow
                    *txlen = 2;
                    tx[0] = 0x40;
                    page_to_be_written = page;
                    block_data_left = 4 - (page % 4);
                    tag.tstate = HT_WRITING_BLOCK_DATA;
                }
            }
            break;
        }
        default: {
            DBG Dbprintf("unknown rxlen: (%i) %02X %02X %02X %02X ...", rxlen, rx[0], rx[1], rx[2], rx[3]);
            break;
        }
    }
}

/*
 * Emulates a Hitag S Tag with the given data from the .hts file
 */
void hts_simulate(bool tag_mem_supplied, int8_t threshold, const uint8_t *data, bool ledcontrol) {
    int overflow = 0;
    uint8_t rx[HITAG_FRAME_LEN] = {0};
    size_t rxlen = 0;
    uint8_t tx[HITAG_FRAME_LEN];
    size_t txlen = 0;

    // free eventually allocated BigBuf memory
    BigBuf_free();
    BigBuf_Clear_ext(false);

    DbpString("Starting Hitag S simulation");

    tag.pstate = HT_READY;
    tag.tstate = HT_NO_OP;

    // read tag data into memory
    if (tag_mem_supplied) {
        DbpString("Loading hitag S memory...");
        memcpy(tag.data.pages, data, HITAGS_MAX_BYTE_SIZE);
    } else {
        // use the last read tag
    }

    // max_page
    update_tag_max_page();

    for (int i = 0; i < tag.max_page; i++) {
        DBG Dbprintf("Page[%2d]: %02X %02X %02X %02X",
                     i,
                     tag.data.pages[i][3],
                     tag.data.pages[i][2],
                     tag.data.pages[i][1],
                     tag.data.pages[i][0]
                    );
    }

    hitag_setup_fpga(0, threshold, ledcontrol);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_EDGE_DETECT);

    while ((BUTTON_PRESS() == false) && (data_available() == false)) {
        uint32_t start_time = 0;

        WDT_HIT();

        // Receive commands from the reader
        hitag_tag_receive_frame(rx, sizeof(rx), &rxlen, &start_time, ledcontrol, &overflow);

        // Check if frame was captured
        if (rxlen > 0) {
            LogTraceBits(rx, rxlen, start_time, TIMESTAMP, true);

            // Disable timer 1 with external trigger to avoid triggers during our own modulation
            AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;

            // Process the incoming frame (rx) and prepare the outgoing frame (tx)
            hts_handle_reader_command(rx, rxlen, tx, &txlen);

            // Wait for HITAG_T_WAIT_RESP carrier periods after the last reader bit,
            // not that since the clock counts since the rising edge, but T_Wait1 is
            // with respect to the falling edge, we need to wait actually (T_Wait1 - T_Low)
            // periods. The gap time T_Low varies (4..10). All timer values are in
            // terms of T0 units
            while (AT91C_BASE_TC0->TC_CV < T0 * (HITAG_T_WAIT_RESP - HITAG_T_LOW)) {};

            // Send and store the tag answer (if there is any)
            if (txlen > 0) {
                // Transmit the tag frame
                start_time = TIMESTAMP;
                hitag_tag_send_frame(tx, txlen, sof_bits, m, ledcontrol);
                LogTraceBits(tx, txlen, start_time, TIMESTAMP, false);
            }

            // Enable and reset external trigger in timer for capturing future frames
            AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

            // Reset the received frame and response timing info
            memset(rx, 0x00, sizeof(rx));

            if (ledcontrol) LED_B_OFF();
        }
        // Reset the frame length
        rxlen = 0;
        // Save the timer overflow, will be 0 when frame was received
        overflow += (AT91C_BASE_TC1->TC_CV / T0);
        // Reset the timer to restart while-loop that receives frames
        AT91C_BASE_TC1->TC_CCR = AT91C_TC_SWTRG;

    }

    hitag_cleanup(ledcontrol);
    // release allocated memory from BigBuff.
    BigBuf_free();

    DbpString("Sim stopped");
}

static int hts_send_receive(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t sizeofrx, size_t *rxlen, int t_wait, bool ledcontrol, bool ac_seq) {
    uint32_t start_time;

    // Send and store the reader command
    // Disable timer 1 with external trigger to avoid triggers during our own modulation
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;

    DBG Dbprintf("tx %d bits:", txlen);
    DBG Dbhexdump((txlen + 7) / 8, tx, false);

    // Wait for HITAG_T_WAIT_SC carrier periods after the last tag bit before transmitting,
    // Since the clock counts since the last falling edge, a 'one' means that the
    // falling edge occurred halfway the period. with respect to this falling edge,
    // we need to wait (T_Wait2 + half_tag_period) when the last was a 'one'.
    // All timer values are in terms of T0 units
    while (AT91C_BASE_TC0->TC_CV < T0 * t_wait) {};

    start_time = TIMESTAMP;

    // Transmit the reader frame
    hitag_reader_send_frame(tx, txlen, ledcontrol, false);

    if (enable_page_tearoff && tearoff_hook() == PM3_ETEAROFF) {
        return PM3_ETEAROFF;
    }

    LogTraceBits(tx, txlen, start_time, TIMESTAMP, true);

    // Enable and reset external trigger in timer for capturing future frames
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

    hts_set_frame_modulation(protocol_mode, ac_seq);

    hitag_reader_receive_frame(rx, sizeofrx, rxlen, &start_time, ledcontrol, m, sof_bits);
    // hts_receive_frame(rx, sizeofrx, rxlen, &start_time, ledcontrol);

    DBG Dbprintf("rx %d bits:", *rxlen);
    DBG Dbhexdump((*rxlen + 7) / 8, rx, false);

    // Check if frame was captured and store it
    if (*rxlen > 0) {
        DBG {
            uint8_t response_bit[sizeofrx * 8];

            for (size_t i = 0; i < *rxlen; i++) {
                response_bit[i] = (rx[i / 8] >> (7 - (i % 8))) & 1;
            }

            Dbprintf("htS: rxlen...... %zu", *rxlen);
            Dbprintf("htS: sizeofrx... %zu", sizeofrx);
            DbpString("htS: response_bit:");
            Dbhexdump(*rxlen, response_bit, false);
        }

        LogTraceBits(rx, *rxlen, start_time, TIMESTAMP, false);
    }

    return PM3_SUCCESS;
}

static int hts_select_tag(const lf_hitag_data_t *packet, uint8_t *tx, size_t sizeoftx, uint8_t *rx, size_t sizeofrx, int t_wait, bool ledcontrol) {
    size_t txlen = 0;
    size_t rxlen = 0;

    // Setup FPGA and initialize
    hitag_setup_fpga(FPGA_LF_EDGE_DETECT_READER_FIELD, 127, ledcontrol);

    // UID request standard   00110
    // UID request Advanced   1100x
    // UID request FAdvanced  11010
    protocol_mode = packet->mode;
    uint8_t cmd = protocol_mode;
    txlen = concatbits(tx, txlen, &cmd, 0, 5, false);
    hts_send_receive(tx, txlen, rx, sizeofrx, &rxlen, t_wait, ledcontrol, true);

    if (rxlen != 32) {
        // DbpString("UID Request failed!");
        return -2;
    }

    memcpy(tag.data.pages[HITAGS_UID_PADR], rx, HITAGS_PAGE_SIZE);

    DBG Dbprintf("UID... %02X%02X%02X%02X", rx[0], rx[1], rx[2], rx[3]);

    // select uid
    txlen = 0;
    cmd = HITAGS_SELECT;
    txlen = concatbits(tx, txlen, &cmd, 0, 5, false);
    txlen = concatbits(tx, txlen, rx, 0, 32, false);
    uint8_t crc = CRC8Hitag1Bits(tx, txlen);
    txlen = concatbits(tx, txlen, &crc, 0, 8, false);

    hts_send_receive(tx, txlen, rx, sizeofrx, &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

    if (rxlen != 32 + (protocol_mode == HITAGS_UID_REQ_STD ? 0 : 8)) {
        DBG Dbprintf("Select UID failed! %i", rxlen);
        return -3;
    }

    memcpy(tag.data.pages[HITAGS_CONFIG_PADR], rx, HITAGS_PAGE_SIZE - 1);

    update_tag_max_page();

    DBG Dbprintf("conf 0: %02X conf 1: %02X conf 2: %02X", tag.data.pages[HITAGS_CONFIG_PADR][0], tag.data.pages[HITAGS_CONFIG_PADR][1], tag.data.pages[HITAGS_CONFIG_PADR][2]);

    if (tag.data.s.config.auth == 1) {

        uint64_t key_le = 0;
        // if the tag is in authentication mode try the key or challenge
        if (packet->cmd == HTSF_KEY) {

            key_le = MemLeToUint6byte(packet->key);

            uint32_t le_val = MemLeToUint4byte(rnd);
            uint64_t state = ht2_hitag2_init(REV64(key_le), REV32(tag.data.s.uid_le), REV32(le_val));

            uint8_t auth_ks[4];
            for (int i = 0; i < 4; i++) {
                auth_ks[i] = ht2_hitag2_byte(&state) ^ 0xff;
            }

            txlen = 0;
            txlen = concatbits(tx, txlen, rnd, 0, 32, false);
            txlen = concatbits(tx, txlen, auth_ks, 0, 32, false);

            DBG DbpString("Authenticating using key:");
            DBG Dbhexdump(6, packet->key, false);
            DBG Dbprintf("%02X %02X %02X %02X %02X %02X %02X %02X", tx[0], tx[1], tx[2], tx[3], tx[4], tx[5], tx[6], tx[7]);

        } else if (packet->cmd == HTSF_CHALLENGE) {

            DBG DbpString("Authenticating using nr,ar pair:");
            DBG Dbhexdump(8, packet->NrAr, false);

            uint64_t NrAr = 0;
            NrAr = ((uint64_t)packet->NrAr[7]) <<  0 |
                   ((uint64_t)packet->NrAr[6]) <<  8 |
                   ((uint64_t)packet->NrAr[5]) << 16 |
                   ((uint64_t)packet->NrAr[4]) << 24 |
                   ((uint64_t)packet->NrAr[3]) << 32 |
                   ((uint64_t)packet->NrAr[2]) << 40 |
                   ((uint64_t)packet->NrAr[1]) << 48 |
                   ((uint64_t)packet->NrAr[0]) << 56;

            txlen = 64;
            for (int i = 0; i < 8; i++) {
                tx[i] = ((NrAr >> (56 - (i * 8))) & 0xFF);
            }

        } else if (packet->cmd == HTSF_82xx) {
            // 8268/8310 Authentication by writing password to block 64

            // send write page request
            txlen = 0;
            cmd = HITAGS_WRITE_PAGE;
            txlen = concatbits(tx, txlen, &cmd, 0, 4, false);

            uint8_t addr = 64;
            txlen = concatbits(tx, txlen, &addr, 0, 8, false);

            crc = CRC8Hitag1Bits(tx, txlen);
            txlen = concatbits(tx, txlen, &crc, 0, 8, false);

            hts_send_receive(tx, txlen, rx, sizeofrx, &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

            if ((rxlen != 2) || (rx[0] >> (8 - 2) != 0x01)) {
                // Dbprintf("no write access on page " _YELLOW_("64") ". not 82xx?");
                return -4;
            }

            txlen = 0;
            txlen = concatbits(tx, txlen, packet->pwd, 0, 32, false);
            crc = CRC8Hitag1Bits(tx, txlen);
            txlen = concatbits(tx, txlen, &crc, 0, 8, false);

            hts_send_receive(tx, txlen, rx, sizeofrx, &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

            if ((rxlen != 2) || (rx[0] >> (8 - 2) != 0x01)) {
                // Dbprintf("write to page " _YELLOW_("64") " failed! wrong password?");
                return -5;
            }

            return 0;
        } else if (packet->cmd == HTSF_PLAIN) {
            // Dbprintf("Error, " _YELLOW_("AUT=1") " This tag is configured in Authentication Mode");
            return -6;
        } else {
            DBG Dbprintf("Error, unknown function: " _RED_("%d"), packet->cmd);
            return -7;
        }

        hts_send_receive(tx, txlen, rx, sizeofrx, &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

        if (rxlen != 32 + (protocol_mode == HITAGS_UID_REQ_STD ? 0 : 8)) {
            DBG Dbprintf("Authenticate failed! " _RED_("%i"), rxlen);
            return -8;
        }

        //encrypted con2,password received.
        DBG Dbprintf("UID... %08X", BSWAP_32(tag.data.s.uid_le));
        DBG Dbprintf("RND... %02X%02X%02X%02X", rnd[0], rnd[1], rnd[2], rnd[3]);

        //decrypt password
        pwdh0 = 0;
        pwdl0 = 0;
        pwdl1 = 0;
        if (packet->cmd == HTSF_KEY) {

            uint32_t le_val = MemLeToUint4byte(rnd);
            uint64_t state = ht2_hitag2_init(REV64(key_le), REV32(tag.data.s.uid_le), REV32(le_val));

            for (int i = 0; i < 4; i++) {
                ht2_hitag2_byte(&state);
            }

            uint8_t con2 = rx[0] ^ ht2_hitag2_byte(&state);
            pwdh0 = rx[1] ^ ht2_hitag2_byte(&state);
            pwdl0 = rx[2] ^ ht2_hitag2_byte(&state);
            pwdl1 = rx[3] ^ ht2_hitag2_byte(&state);

            DBG Dbprintf("con2 %02X pwdh0 %02X pwdl0 %02X pwdl1 %02X", con2, pwdh0, pwdl0, pwdl1);
        }
    }
    return 0;
}

/*
 * Authenticates to the Tag with the given key or challenge.
 * If the key was given the password will be decrypted.
 * Reads every page of a hitag S transpoder.
 */
void hts_read(const lf_hitag_data_t *payload, bool ledcontrol) {

    uint8_t rx[HITAG_FRAME_LEN] = { 0x00 };
    uint8_t tx[HITAG_FRAME_LEN] = { 0x00 };

    int status = PM3_SUCCESS, reason = -1;
    reason = hts_select_tag(payload, tx, ARRAYLEN(tx), rx, ARRAYLEN(rx), HITAG_T_WAIT_FIRST, ledcontrol);
    if (reason != 0) {
        status = PM3_ERFTRANS;
        goto read_end;
    }


    if (payload->page >= tag.max_page) {
        DBG Dbprintf("Warning, read page "_YELLOW_("%d") " > max page("_YELLOW_("%d") ") ", payload->page, tag.max_page);
    }

    int page_addr = payload->page;
    int page_index = 0;
    lf_hts_read_response_t card = {0};

    memcpy(card.config_page.asBytes, tag.data.pages[HITAGS_CONFIG_PADR], HITAGS_PAGE_SIZE);

    while ((BUTTON_PRESS() == false) && (data_available() == false)) {

        if (payload->page_count == 0) {
            if (page_addr > tag.max_page) break;
        } else if (page_addr > 255 || page_addr >= payload->page + payload->page_count) {
            break;
        }

        WDT_HIT();

        size_t rxlen = 0;

        //send read request
        size_t txlen = 0;
        uint8_t cmd = HITAGS_READ_PAGE;
        txlen = concatbits(tx, txlen, &cmd, 0, 4, false);
        uint8_t addr = page_addr;
        txlen = concatbits(tx, txlen, &addr, 0, 8, false);
        uint8_t crc = CRC8Hitag1Bits(tx, txlen);
        txlen = concatbits(tx, txlen, &crc, 0, 8, false);

        hts_send_receive(tx, txlen, rx, ARRAYLEN(rx), &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

        if (rxlen != 32 + (protocol_mode == HITAGS_UID_REQ_STD ? 0 : 8)) {
            DBG Dbprintf("Read page failed!");
            card.pages_reason[page_index] = -11;
            // status = PM3_ERFTRANS;
            // goto read_end;
            page_addr++;
            page_index++;
            continue;
        }

        //save received data - 40 bits
        memcpy(card.pages[page_index], rx, HITAGS_PAGE_SIZE);

        if (g_dbglevel >= DBG_EXTENDED) {
            if (page_addr == 1 && (payload->cmd == HTSF_KEY || payload->cmd == HTSF_CHALLENGE) && card.config_page.s.auth == 1) {
                DBG Dbprintf("Page[%2d]: %02X %02X %02X %02X", page_addr,
                             card.pages[page_index][0],
                             card.pages[page_index][1],
                             card.pages[page_index][2],
                             pwdh0);
            } else {  // HTSF_PLAIN or HTSF_82xx can read the full page
                DBG Dbprintf("Page[%2d]: %02X %02X %02X %02X", page_addr,
                             card.pages[page_index][0],
                             card.pages[page_index][1],
                             card.pages[page_index][2],
                             card.pages[page_index][3]);
            }
        }

        page_addr++;
        page_index++;
        //display key and password if possible
        if (page_addr == 2 && card.config_page.s.auth == 1 && card.config_page.s.LKP) {
            if (payload->cmd == HTSF_KEY) {
                DBG Dbprintf("Page[ 2]: %02X %02X %02X %02X",
                             payload->key[1],
                             payload->key[0],
                             pwdl1,
                             pwdl0
                            );
                DBG Dbprintf("Page[ 3]: %02X %02X %02X %02X",
                             payload->key[5],
                             payload->key[4],
                             payload->key[3],
                             payload->key[2]
                            );
                card.pages_reason[page_index++] = 1;
                card.pages_reason[page_index++] = 1;
            } else {
                //if the authentication is done with a challenge the key and password are unknown
                DBG Dbprintf("Page[ 2]: __ __ __ __");
                DBG Dbprintf("Page[ 3]: __ __ __ __");
                card.pages_reason[page_index++] = -11;
                card.pages_reason[page_index++] = -11;
            }
            // since page 2+3 are not accessible when LKP == 1 and AUT == 1 fastforward to next readable page
            page_addr = 4;
        }
    }

read_end:
    hitag_cleanup(ledcontrol);
    reply_reason(CMD_LF_HITAGS_READ, status, reason, (uint8_t *)&card, sizeof(card));
}

/*
 * Authenticates to the Tag with the given Key or Challenge.
 * Writes the given 32Bit data into page_
 */
void hts_write_page(const lf_hitag_data_t *payload, bool ledcontrol) {

    //check for valid input
    if (payload->page == 0) {
        DBG Dbprintf("Warning, write page 0");
    }

    uint8_t rx[HITAG_FRAME_LEN];
    size_t rxlen = 0;

    uint8_t tx[HITAG_FRAME_LEN];
    size_t txlen = 0;

    int status = PM3_ESOFT, reason = -1;
    reason = hts_select_tag(payload, tx, ARRAYLEN(tx), rx, ARRAYLEN(rx), HITAG_T_WAIT_FIRST, ledcontrol);
    if (reason != 0) {
        status = PM3_ERFTRANS;
        goto write_end;
    }

    //check if the given page exists
    if (payload->page > tag.max_page) {
        DBG Dbprintf("Warning, page number too large");
        // 82xx CON0 is fully modifiable
    }

    //send write page request
    txlen = 0;

    uint8_t cmd = HITAGS_WRITE_PAGE;
    txlen = concatbits(tx, txlen, &cmd, 0, 4, false);

    uint8_t addr = payload->page;
    txlen = concatbits(tx, txlen, &addr, 0, 8, false);

    uint8_t crc = CRC8Hitag1Bits(tx, txlen);
    txlen = concatbits(tx, txlen, &crc, 0, 8, false);

    hts_send_receive(tx, txlen, rx, ARRAYLEN(rx), &rxlen, HITAG_T_WAIT_SC, ledcontrol, false);

    if ((rxlen != 2) || (rx[0] >> (8 - 2) != 0x01)) {
        DBG Dbprintf("no write access on page " _YELLOW_("%d"), payload->page);
        reason = -9;
        goto write_end;
    }

    // //ACK received to write the page. send data
    // uint8_t data[4] = {0, 0, 0, 0};
    // switch (payload->cmd) {
    //     case HTSF_PLAIN:
    //     case HTSF_CHALLENGE:
    //     case HTSF_KEY:
    //         data[0] = payload->data[3];
    //         data[1] = payload->data[2];
    //         data[2] = payload->data[1];
    //         data[3] = payload->data[0];
    //         break;
    //     default: {
    //         res = PM3_EINVARG;
    //         goto write_end;
    //     }
    // }

    txlen = 0;
    txlen = concatbits(tx, txlen, payload->data, 0, 32, false);
    crc = CRC8Hitag1Bits(tx, txlen);
    txlen = concatbits(tx, txlen, &crc, 0, 8, false);

    enable_page_tearoff = g_tearoff_enabled;

    if (hts_send_receive(tx, txlen, rx, ARRAYLEN(rx), &rxlen, HITAG_T_WAIT_SC, ledcontrol, false) == PM3_ETEAROFF) {
        status = PM3_ETEAROFF;
        enable_page_tearoff = false;
        goto write_end;
    }

    if ((rxlen != 2) || (rx[0] >> (8 - 2) != 0x01)) {
        reason = -10;  // write failed
    } else {
        status = PM3_SUCCESS;
    }

write_end:
    hitag_cleanup(ledcontrol);
    reply_reason(CMD_LF_HITAGS_WRITE, status, reason, NULL, 0);
}

int hts_read_uid(uint32_t *uid, bool ledcontrol, bool send_answer) {
    // Setup FPGA and initialize
    hitag_setup_fpga(FPGA_LF_EDGE_DETECT_READER_FIELD, 127, ledcontrol);

    protocol_mode = HITAGS_UID_REQ_ADV1;
    uint8_t cmd = protocol_mode;

    size_t rxlen = 0;
    uint8_t rx[HITAG_FRAME_LEN] = { 0x00 };

    size_t txlen = 0;
    uint8_t tx[HITAG_FRAME_LEN] = { 0x00 };

    txlen = concatbits(tx, txlen, &cmd, 0, 5, false);

    hts_send_receive(tx, txlen, rx, ARRAYLEN(rx), &rxlen, HITAG_T_WAIT_FIRST, ledcontrol, true);

    int status = PM3_SUCCESS;
    if (rxlen == 32) {

        memcpy(tag.data.pages[0], rx, HITAGS_PAGE_SIZE);

        if (uid) {
            *uid = BSWAP_32(tag.data.s.uid_le);
        }

    } else {
        DBG DbpString("UID Request failed!");
        status = PM3_ERFTRANS;
    }

    hitag_cleanup(ledcontrol);
    if (send_answer) {
        reply_ng(CMD_LF_HITAGS_UID, status, (uint8_t *)tag.data.pages, sizeof(tag.data.pages));
    }
    return status;
}

/*
 * Tries to authenticate to a Hitag S Transponder with the given challenges from a .cc file.
 * Displays all Challenges that failed.
 * When collecting Challenges to break the key it is possible that some data
 * is not received correctly due to Antenna problems. This function
 * detects these challenges.
 */
void hts_check_challenges(const uint8_t *data, uint32_t datalen, bool ledcontrol) {

    //check for valid input
    if (datalen < 8) {
        DBG Dbprintf("Error, missing challenges");
        reply_ng(CMD_LF_HITAGS_TEST_TRACES, PM3_EINVARG, NULL, 0);
        return;
    }
    uint32_t dataoffset = 0;

    uint8_t rx[HITAG_FRAME_LEN];
    uint8_t tx[HITAG_FRAME_LEN];

    while ((BUTTON_PRESS() == false) && (data_available() == false)) {
        // Watchdog hit
        WDT_HIT();

        lf_hitag_data_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.cmd = HTSF_CHALLENGE;

        memcpy(payload.NrAr, data + dataoffset, 8);

        int reason = hts_select_tag(&payload, tx, ARRAYLEN(tx), rx, ARRAYLEN(rx), HITAG_T_WAIT_FIRST, ledcontrol);

        DBG  Dbprintf("Challenge %s: %02X %02X %02X %02X %02X %02X %02X %02X",
                      reason != 0 ? "failed " : "success",
                      payload.NrAr[0], payload.NrAr[1],
                      payload.NrAr[2], payload.NrAr[3],
                      payload.NrAr[4], payload.NrAr[5],
                      payload.NrAr[6], payload.NrAr[7]
                     );

        if (reason != 0) {
            // Need to do a dummy UID select that will fail
            FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
            SpinDelay(2);
            hts_select_tag(&payload, tx, ARRAYLEN(tx), rx, ARRAYLEN(rx), HITAG_T_WAIT_FIRST, ledcontrol);
        }

        dataoffset += 8;
        if (dataoffset >= datalen - 8) {
            break;
        }
        // reset field
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);

        // min t_reset = 2ms
        SpinDelay(2);
    }

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_HITAGS_TEST_TRACES, PM3_SUCCESS, NULL, 0);
    return;
}
