#include "protocol.h"

#include <string.h>

int idti_header_parse(const uint8_t *buf, size_t len, IdtiHeader *out)
{
    if (len < IDTI_HEADER_LEN_V2)
    {
        return -1;
    }
    if (buf[0] != IDTI_STX)
    {
        return -1;
    }
    if (buf[6] != IDTI_HEADER_LEN_V2) /* Header Length: V2/V3(44byte)만 지원 */
    {
        return -1;
    }

    uint8_t checksum = 0;
    for (size_t i = 0; i < 31; i++) /* STX ~ SubCommand */
    {
        checksum ^= buf[i];
    }
    if (checksum != buf[31])
    {
        return -1;
    }

    uint16_t packet_length = ((uint16_t)buf[1] << 8) | buf[2];
    uint16_t frame_option   = ((uint16_t)buf[4] << 8) | buf[5];
    size_t tail_len = (frame_option & IDTI_FOPT_CHECK_PACKET) ? 4 : 2;

    if (packet_length < (uint16_t)(IDTI_HEADER_LEN_V2 + tail_len))
    {
        return -1;
    }

    out->packet_length = packet_length;
    out->protocol_version = buf[3];
    out->frame_option = frame_option;
    memcpy(out->dest_addr, &buf[7], IDTI_ADDR_DEST_LEN);
    memcpy(out->src_addr, &buf[15], IDTI_ADDR_SRC_LEN);
    out->frame_index = ((uint32_t)buf[20] << 24) | ((uint32_t)buf[21] << 16) |
                        ((uint32_t)buf[22] << 8)  | (uint32_t)buf[23];
    out->password     = ((uint32_t)buf[24] << 24) | ((uint32_t)buf[25] << 16) |
                        ((uint32_t)buf[26] << 8)  | (uint32_t)buf[27];
    out->command = buf[29];
    out->sub_command = buf[30];
    out->object = buf[33];
    out->start_item = buf[34];
    out->end_item = buf[35];
    out->data_block_current = ((uint16_t)buf[36] << 8) | buf[37];
    out->data_block_end     = ((uint16_t)buf[38] << 8) | buf[39];
    out->data_block_total   = ((uint16_t)buf[40] << 8) | buf[41];
    out->data_block_one_len = ((uint16_t)buf[42] << 8) | buf[43];
    out->data_len = (size_t)packet_length - IDTI_HEADER_LEN_V2 - tail_len;

    return 0;
}

int idti_build_packet(uint8_t *out, size_t out_cap,
                       const uint8_t dest_addr[IDTI_ADDR_DEST_LEN],
                       const uint8_t src_addr[IDTI_ADDR_SRC_LEN],
                       uint16_t frame_option,
                       uint32_t frame_index, uint32_t password,
                       uint8_t command, uint8_t sub_command, uint8_t object,
                       uint8_t start_item, uint8_t end_item,
                       uint16_t data_current, uint16_t data_end,
                       uint16_t data_total, uint16_t data_one_len,
                       const uint8_t *data, size_t data_len)
{
    const size_t tail_len = 2; /* CheckBytes를 만들지 않으므로 Tail은 항상 2byte */

    /* IsRequestAck는 세우고 IsCheckPacket은 지운다 (Tail 2byte 고정) */
    frame_option = (uint16_t)((frame_option | IDTI_FOPT_REQUEST_ACK) & ~IDTI_FOPT_CHECK_PACKET);
    const size_t total_len = IDTI_HEADER_LEN_V2 + data_len + tail_len;

    if (total_len > out_cap || total_len > 0xFFFF)
    {
        return -1;
    }

    uint8_t *p = out;
    p[0] = IDTI_STX;
    p[1] = (uint8_t)(total_len >> 8);
    p[2] = (uint8_t)(total_len & 0xFF);
    p[3] = IDTI_PROTOCOL_VERSION_V2;
    p[4] = (uint8_t)(frame_option >> 8);
    p[5] = (uint8_t)(frame_option & 0xFF);
    p[6] = IDTI_HEADER_LEN_V2;
    memcpy(&p[7], dest_addr, IDTI_ADDR_DEST_LEN);
    memcpy(&p[15], src_addr, IDTI_ADDR_SRC_LEN);
    p[20] = (uint8_t)(frame_index >> 24);
    p[21] = (uint8_t)(frame_index >> 16);
    p[22] = (uint8_t)(frame_index >> 8);
    p[23] = (uint8_t)(frame_index);
    p[24] = (uint8_t)(password >> 24);
    p[25] = (uint8_t)(password >> 16);
    p[26] = (uint8_t)(password >> 8);
    p[27] = (uint8_t)(password);
    p[28] = 0x00; /* Command Option: fixed */
    p[29] = command;
    p[30] = sub_command;

    uint8_t checksum = 0;
    for (size_t i = 0; i < 31; i++)
    {
        checksum ^= p[i];
    }
    p[31] = checksum;

    p[32] = 0x00; /* Object Type: fixed */
    p[33] = object;
    p[34] = start_item;
    p[35] = end_item;
    p[36] = (uint8_t)(data_current >> 8);
    p[37] = (uint8_t)(data_current & 0xFF);
    p[38] = (uint8_t)(data_end >> 8);
    p[39] = (uint8_t)(data_end & 0xFF);
    p[40] = (uint8_t)(data_total >> 8);
    p[41] = (uint8_t)(data_total & 0xFF);
    p[42] = (uint8_t)(data_one_len >> 8);
    p[43] = (uint8_t)(data_one_len & 0xFF);

    if (data_len > 0 && data != NULL)
    {
        memcpy(&p[IDTI_HEADER_LEN_V2], data, data_len);
    }

    size_t tail_off = IDTI_HEADER_LEN_V2 + data_len;
    p[tail_off]     = IDTI_PACKET_CHECKSUM_FIXED;
    p[tail_off + 1] = IDTI_ETX;

    return (int)total_len;
}

uint8_t idti_to_bcd(int value)
{
    int v = value % 100;
    if (v < 0)
    {
        v = 0;
    }
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}
