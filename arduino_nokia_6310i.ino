/*
This code sends (on boot) an SMS with the context of the TEXT #define 
to the RECIPIENT, using the SMS Center SMSC (Please don't forget
to set those first!!!)

For details on the connections and how this works in a high level, please
view my post at https://ilias.giechaskiel.com/posts/arduino_sms/index.html 

The code was heavily based on the http://www.gnokii.org/ library, mostly
functions NK6510_SendSMS and sms_encode of common/phones/nk6510.c and
fbus_send_message and fbus_txt_send_frame of common/links/fbus.c


There are two points where this code is brittle, denoted by FIXME.
Both of them have to do with lengths (one of the SMSC, and the other
of the actual content of the text). Because it was hard to trace where
the values come from in gnokii, I set them manually based on the debug
output for the same message. 

In other words, if the code below fails, create a text file containing
the message you want to send, and run the command
gnokii.exe --sendsms $RECIPIENT < $TEXT
where the variables are set appropriate, e.g. to +XXXXXXXXXXXX and
text.txt respectively.
*/

#define FBUS_TRANSMIT_MAX_LENGTH 256
#define BAUD 115200
#define phone Serial1

/* Phones should have the +XX country code*/
#define SMSC ""
#define RECIPIENT ""
#define TEXT "Sending texts from my Arduino is awesome!"

#define DEBUG

boolean first;
unsigned int request_sequence_number;

unsigned char smsc[20];
unsigned char rec[20];

#define HEADER_BYTES 15
#define LEN_POS 5
unsigned char sms[FBUS_TRANSMIT_MAX_LENGTH] = {0x1E, /* Frame start */
                                               0x00, /* Dst=Phone */
                                               0x0C, /* Src=Serial */
                                               0x02, /* Message Type=SMS */
                                               0x00, /* Length Byte 1 */
                                               0x00, /* Length Byte 2 (To be set) */
                                               /* Further FBus header */
                                               0x00, 
                                               0x01, 
                                               0x00,
                                               /*  Send SMS Header */
                                               0x02, 
                                               0x00, 
                                               0x00, 
                                               0x00, 
                                               0x55, 
                                               0x55
                                               }; 


void setup()
{
   Serial.begin(BAUD);
   phone.begin(BAUD);
   
   char_semi_octet_pack(SMSC, smsc + 1);
   
   /* FIXME: Not sure why the return value above is not used,
      but this works at least with my Greek number */
   smsc[0] = 0x06;
   
   request_sequence_number = 0;
   first = true;
   
   /* Initialise the Fbus by sending "U" 128 times */
   for (int x = 0; x < 128; x++)
      phone.write("U");
      
   send_sms(RECIPIENT, TEXT);
}

void loop()
{
}

void send_sms(char* num, char* text)
{
    unsigned int numBytes;
    
    rec[0] = char_semi_octet_pack(num, rec + 1);
    
    memset(sms + HEADER_BYTES, 0, FBUS_TRANSMIT_MAX_LENGTH - HEADER_BYTES);
    numBytes = HEADER_BYTES + sms_encode(text, sms + HEADER_BYTES);
    sms[LEN_POS] = numBytes - 6 + 2; /* Excludes initial header, but includes part of footer*/
    numBytes = create_footer(sms, numBytes);

    /* Send the full frame to the phone */
    for (unsigned char j = 0; j < numBytes; j++) {
      #ifdef DEBUG      
      Serial.print(sms[j], HEX);
      Serial.print(" ");
      #endif
      
      phone.write(sms[j]);
    }
    
    #ifdef DEBUG
    Serial.println();
    #endif
    
    phone.flush();
}

static int sms_encode(char* text, unsigned char *req)
{
    int text_length = strlen(text);
    unsigned char c, w, n, shift = 0, bittext_length = 0;
    int pos = 0, udh_length_pos, len;
    
    req[pos++] = 0x01; /* one big block */
    req[pos++] = 0x02; /* message type: submit */
    req[pos++] = 0x00; /* length (will be set at the end) */
    req[pos++] = 0x01 | 0x10; /* SMS Submit  | validity indicator */
    req[pos++] = 0x00; /* reference */
    req[pos++] = 0x00; /* pid */
    req[pos++] = 0x00; /* dcs */
    req[pos++] = 0x00; /* fixed byte */
    req[pos++] = 0x04; /* total blocks */
    
    /* Block 1. Remote Number */
    len = rec[0] + 4;
    if (len % 2) len++;
    len /= 2;
    req[pos] = 0x82; /* type: number */
    req[pos + 1] = (len + 4 > 0x0c) ? (len + 4) : 0x0c; /* offset to next block starting from start of block (req[18]) */
    req[pos + 2] = 0x01; /* first number field => remote_number */
    req[pos + 3] = len; /* actual data length in this block */
    memcpy(req + pos + 4, rec, len);
    pos += ((len + 4 > 0x0c) ? (len + 4) : 0x0c);
    	
    /* Block 2. SMSC Number */
    len = smsc[0] + 1;
    req[pos] = 0x82; /* type: number */
    req[pos + 1] = (len + 4 > 0x0c) ? (len + 4) : 0x0c; /* offset to next block starting from start of block (req[18]) */
    req[pos + 2] = 0x02; /* first number field => remote_number */
    req[pos + 3] = len;
    memcpy(req + pos + 4, smsc, len);
    pos += ((len + 4 > 0x0c) ? (len + 4) : 0x0c);
    
    /* Block 3. User Data */
    req[pos++] = 0x80; /* type: User Data */
     
    for (n = 0; n < text_length; n++) {
       unsigned char c = text[n] & 0x7f;
       c >>= shift;
       w = text[n+1] & 0x7f;
       w <<= (7-shift);
       shift += 1;
       c = c | w;
       if (shift == 7) {
         shift = 0x00;
         n++;
       }
       req[pos + 3 + bittext_length] = c;
       bittext_length++;
    }
    
    /* FIXME: I am not sure why these are different */
    req[pos++] = bittext_length + 4;
    req[pos++] = bittext_length;
    req[pos++] = bittext_length;
    pos += bittext_length;
    
    udh_length_pos = pos - bittext_length - 3;
     
    /* padding */
    if (req[udh_length_pos] % 8 != 0) {
        memcpy(req + pos, "\x55\x55\x55\x55\x55\x55\x55\x55", 8 - req[udh_length_pos] % 8);
    	pos += 8 - req[udh_length_pos] % 8;
    	req[udh_length_pos] += 8 - req[udh_length_pos] % 8;
    }
    
    /* Block 4. Validity Period */
    req[pos++] = 0x08; /* type: validity */
    req[pos++] = 0x04; /* block length */
    req[pos++] = 0x01; /* data length */
    req[pos++] = 0xA9; /* validity */
    req[2] = pos - 1;
    return pos;
}

int char_semi_octet_pack(char *number, unsigned char *output)
{
    char *in_num = number;  /* Pointer to the input number */
    unsigned char *out_num = output; /* Pointer to the output */
    int count = 0; /* This variable is used to notify us about count of already
	              packed numbers. */

    /* The first byte in the Semi-octet representation of the address field is
       the Type-of-Address. This field is described in the official GSM
       specification 03.40 version 6.1.0, section 9.1.2.5, page 33. We support
       only international, unknown and alphanumeric number. */

    *out_num++ = 0x91; /* international */

    if (*in_num == '+') in_num++; /* skip leading '+' */

    /* The next field is the number. It is in semi-octet representation - see
       GSM specification 03.40 version 6.1.0, section 9.1.2.3, page 31. */
    while (*in_num) {
        if (count & 0x01) {
            *out_num = *out_num | ((*in_num - '0') << 4);
	    out_num++;
        }
	else
	    *out_num = *in_num - '0';
	count++; in_num++;
    }

    /* We should also fill in the most significant bits of the last byte with
       0x0f (1111 binary) if the number is represented with odd number of
       digits. */
    if (count & 0x01) {
        *out_num = *out_num | 0xf0;
	out_num++;
    }
    return (2 * (out_num - output - 1) - (count % 2));
}

int create_footer(unsigned char* buffer, unsigned int len)
{
  int count, seqnum = 0;
  unsigned char checksum;
  buffer[len++] = 0x01; /* This is the last frame */
    
  seqnum = 0x40 + request_sequence_number;
  request_sequence_number = (request_sequence_number + 1) & 0x07;
  /*  For the very first time sequence number should be ORed with
      0x20. It should initialize sequence counter in the phone. */
  if (first) {
      seqnum |= 0x20;
      first = false;
  }
    
  buffer[len++] = seqnum;
   
  /* If the message length is odd we should add pad byte 0x00 */
  if (len % 2)
      buffer[len++] = 0x00;

  /* Now calculate checksums over entire message and append to message. */

  /* Odd bytes */
  checksum = 0;
  for (count = 0; count < len; count += 2)
      checksum ^= buffer[count];

  buffer[len++] = checksum;

  /* Even bytes */
  checksum = 0;
  for (count = 1; count < len; count += 2)
      checksum ^= buffer[count];

  buffer[len++] = checksum;
  
  return len;
}

