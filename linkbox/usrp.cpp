#include "usrp.h"

#include "common.h"
#include "main.h"
#include "hostfile.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/stat.h>

CUSRP::CUSRP()
{

}


CUSRP::~CUSRP()
{

}


int CUSRP::Init(char *AudioDevice)
{
    int Ret = 0;


    char AddressBuf[256];
    int PortIn = 0;
    int PortOut = 0;

    // hack better parsing
    char *PortBegin = strstr(AudioDevice, ":");
    *PortBegin++ = '\0';
    sscanf(AudioDevice, "USRP/%s", AddressBuf);
    sscanf(PortBegin, "%u:%u", &PortIn, &PortOut);

    // todo error handling.
    LOG_NORM(("%s#%d: USRP \"%s:%u, output %u\"\n",__FUNCTION__,__LINE__,AddressBuf, PortIn, PortOut));

    // In
    if ((InSock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("Socket");
        return ERR_USRP_FAIL;
    }

    InServerAddr.sin_family = AF_INET;
    InServerAddr.sin_port = htons(PortIn);
    InServerAddr.sin_addr.s_addr = INADDR_ANY;
    bzero(&(InServerAddr.sin_zero),8);

    struct timeval InReadTimeout;
    InReadTimeout.tv_sec = 0;
    InReadTimeout.tv_usec = 10;

    if (bind(InSock,(struct sockaddr *)&InServerAddr,
    sizeof(struct sockaddr)) == -1)
    {
        perror("Bind");
        return ERR_USRP_FAIL;
    }

    // Out
    int OutSock,OutBytesRecv,OutSinSize;
    struct sockaddr_in OutServerAddr;
    struct hostent *OutHost;
    char OutSendData[1024],OutRecvData[1024];
    OutHost = GetHostByName(AddressBuf);

    if ((OutSock = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
    {
        perror("socket");
        return ERR_USRP_FAIL;
    }

    OutServerAddr.sin_family = AF_INET;
    OutServerAddr.sin_port = htons(PortOut);
    OutServerAddr.sin_addr = *((struct in_addr *)OutHost->h_addr);
    bzero(&(OutServerAddr.sin_zero),8);
    OutSinSize = sizeof(struct sockaddr);
    
    if(pthread_create( &RecvThread, NULL, &CUSRP::StaticRecvMain, this ) < 0) {
        perror("socket");
        Ret = ERR_USRP_FAIL; // todo better error;
    }

    return Ret;
}


//static
void* CUSRP::StaticSendMain( void* pParam )
{
    return ((CUSRP*)pParam)->SendMain();
}


//static
void* CUSRP::StaticRecvMain( void* pParam )
{
    return ((CUSRP*)pParam)->RecvMain();
}


void* CUSRP::SendMain()
{
    char OutRecvData[1024];
    USRPHeader *UsrpHeader = reinterpret_cast<USRPHeader *>(OutRecvData);
    memcpy(UsrpHeader->Usrp, "USRP", 4);
    UsrpHeader->SequenceNum = 0;
    UsrpHeader->Memory = 0;
    UsrpHeader->Ptt = 0;
    UsrpHeader->Talkgroup = 0;
    UsrpHeader->Type = USRP_TYPE_VOICE;
    UsrpHeader->Mpxid = 0;
    UsrpHeader->Reserved = 0;
    

    return NULL;
}

void* CUSRP::RecvMain()
{
    int InBytesRead;
    char InRecvData[1024];
    USRPHeader *UsrpHeader = reinterpret_cast<USRPHeader *>( InRecvData );
    short *UsrpVoiceFrame = reinterpret_cast<short *>(UsrpHeader + 1);

    do {
        InBytesRead = recvfrom(InSock, InRecvData, 1024,0,
                        (struct sockaddr *)&InClientAddr, &InAddrLen);

        // Since it's coming over UDP, we expect at least 32 bytes.
        if(InBytesRead >= 32) {
            if(memcmp(UsrpHeader->Usrp, "USRP", 4) != 0) {
                LOG_NORM(("%s#%d: USRP Packet invalid, size %i\n",__FUNCTION__,__LINE__, InBytesRead));
                continue;
            }

            // Check sequence validity.
            if((int)(ntohl(UsrpHeader->SequenceNum) - InSequenceNum) <= 0) {
                LOG_NORM(("%s#%d: USRP Packet out of sequence, size %i\n",__FUNCTION__,__LINE__, InBytesRead));
                continue;
            }

            switch(UsrpHeader->Type)
            {
            case USRP_TYPE_VOICE:
                memcpy(&InAudioBuffer[InAudioBufferWriteOff], UsrpVoiceFrame, USRP_VOICE_FRAME_SIZE * sizeof(short));
                InAudioBufferWriteOff = (InAudioBufferWriteOff + USRP_VOICE_FRAME_SIZE) % USRP_VOICE_BUFFER_SIZE;
                break;

            case USRP_TYPE_DTMF:
                LOG_NORM(("%s#%d: USRP_TYPE_DTMF unimplemented %i bytes\n",__FUNCTION__,__LINE__, InBytesRead));
                break;

            case USRP_TYPE_TEXT:
                LOG_NORM(("%s#%d: USRP_TYPE_TEXT unimplemented %i bytes\n",__FUNCTION__,__LINE__, InBytesRead));
                break;
            }
        }
    } while (InBytesRead > 0);

    return NULL;
}
