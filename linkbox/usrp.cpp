#include "usrp.h"

#include "common.h"
#include "main.h"
#include "hostfile.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <cmath>

#include <mutex>

struct CUSRPData
{
    uint32_t SequenceNum = 0;
    short AudioBuffer[USRP_VOICE_BUFFER_FRAME_SIZE];        // 8kHz 16-bit signed
    int AudioBufferWriteOff = 0;
    int AudioBufferReadOff = 0;
    int AudioFrames = 0;                                    
    USRPHeader Header = {};                                 // TODO: Buffer...?


    mutable std::mutex Mutex;

    USRPHeader GetHeader() const
    {
        std::lock_guard<std::mutex> Lock( Mutex );
        return Header;
    }

    int Read(short* FrameData, size_t BytesSize)
    {
        std::lock_guard<std::mutex> Lock( Mutex );
        
        const int READ_SIZE = USRP_VOICE_FRAME_SIZE * sizeof(short); 
        assert(BytesSize >= READ_SIZE);

        if( AudioFrames >= BytesSize )
        {
            memcpy(FrameData, &AudioBuffer[AudioBufferReadOff], BytesSize);
            AudioBufferWriteOff = (AudioBufferWriteOff + BytesSize) % USRP_VOICE_BUFFER_FRAME_SIZE;
            AudioFrames -= BytesSize / sizeof(short);
            return BytesSize;
        }
        return 0;
    }

    int Write(const short* FrameData, size_t BytesSize)
    {
        std::lock_guard<std::mutex> Lock( Mutex );

        if( AudioFrames < USRP_VOICE_BUFFER_FRAME_SIZE * sizeof(short) )
        {
            memcpy(&AudioBuffer[AudioBufferWriteOff], FrameData, BytesSize);
            AudioBufferWriteOff = (AudioBufferWriteOff + BytesSize) % USRP_VOICE_BUFFER_FRAME_SIZE;
            AudioFrames += BytesSize / sizeof(short);
            return BytesSize;
        }
        return 0;
    }


};

CUSRP::CUSRP()
{
    InData = new CUSRPData;
    OutData = new CUSRPData;
}


CUSRP::~CUSRP()
{
    delete InData;
    delete OutData;
}


int CUSRP::Init(const char *NodeName, char *AudioDevice, ClientInfo *pAudioC)
{
    int Ret = 0;

    char AddressBuf[256];
    PortIn = 0;
    PortOut = 0;

    AudioC = pAudioC;

    // hack better parsing
    char *PortBegin = strstr(AudioDevice, ":");
    *PortBegin++ = '\0';
    sscanf(AudioDevice, "USRP/%s", AddressBuf);
    sscanf(PortBegin, "%u:%u", &PortIn, &PortOut);

    // todo error handling.
    LOG_NORM(("%s#%d: USRP \"%s:%u, output %u\"\n",__FUNCTION__,__LINE__,AddressBuf, PortIn, PortOut));

    // Create & open a named pipe.
    char PipeName[256];
    sprintf(PipeName, "/tmp/usrp_pipe_%s_%u_%u", NodeName, PortIn, PortOut);
    mkfifo(PipeName, O_RDWR);
    chmod(PipeName, 0666);

    if((pAudioC->Socket = open(PipeName,0666)) < 0) {
        LOG_ERROR(("%s#%d: open(\"%s\") failed: %s",__FUNCTION__,__LINE__,PipeName,
                        Err2String(errno)));
        Ret = ERR_AUDIO_DEV_OPEN;
    }


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


bool CUSRP::PollCOS()
{
    return InData->GetHeader().Ptt;
}


int CUSRP::Read(short *OutData, int MaxRead)
{
    const int READ_SIZE = USRP_VOICE_FRAME_SIZE * sizeof(short); 
    assert(MaxRead >= READ_SIZE);

    // We prob only want to read from the socket, but keep an internal buffer
    // whilst iterating for now.
    if(InData->Read(OutData, MaxRead) > 0)
    {
        size_t ReadSize = read(AudioC->Socket, OutData, MaxRead);
        InData->AudioFrames -= ReadSize / sizeof(short);
        return ReadSize;
    }
    return 0;
}


int CUSRP::Write(const short *FrameData, int SizeBytes)
{
    return OutData->Write(FrameData, SizeBytes);
}


//static
void* CUSRP::StaticRecvMain( void* pParam )
{
    return ((CUSRP*)pParam)->RecvMain();
}


void* CUSRP::RecvMain()
{
    int InBytesRead;
    char InRecvData[1024];
    USRPHeader *UsrpHeader = reinterpret_cast<USRPHeader *>( InRecvData );
    short *UsrpVoiceFrame = reinterpret_cast<short *>(UsrpHeader + 1);

    do {
        if(PortOut != 1337)
        {
            InBytesRead = recvfrom(InSock, InRecvData, 1024,0,
                            (struct sockaddr *)&InClientAddr, &InAddrLen);
        }
        else
        {
            InBytesRead = USRP_VOICE_BUFFER_FRAME_SIZE * sizeof(short);
            UsrpHeader->Usrp[0] = 'U';
            UsrpHeader->Usrp[1] = 'S';
            UsrpHeader->Usrp[2] = 'R';
            UsrpHeader->Usrp[3] = 'P';
            UsrpHeader->SequenceNum = htonl(ntohl(UsrpHeader->SequenceNum) + 1);
            UsrpHeader->Memory = 0;
            UsrpHeader->Ptt = 1;
            UsrpHeader->Talkgroup = 1337;
            UsrpHeader->Type = USRP_TYPE_VOICE;
            UsrpHeader->Mpxid = 0;
            UsrpHeader->Reserved = 0;

            for(int i = 0; i < USRP_VOICE_FRAME_SIZE; ++i)
            {
                float val = ( (float)i / (float)USRP_VOICE_FRAME_SIZE ) * 3.14159*2.0;
                float val2 = sin(val);
                UsrpVoiceFrame[i] = (short)(sin( val ) * 15000.0f);
            }

            usleep(2000000);

            while( InData->AudioFrames > 2 * USRP_VOICE_FRAME_SIZE )
            {
                usleep(20000);
            }
        }

        // Since it's coming over UDP, we expect at least 32 bytes.
        if(InBytesRead >= 32) {
            if(memcmp(UsrpHeader->Usrp, "USRP", 4) != 0) {
                LOG_NORM(("%s#%d: USRP Packet invalid, size %i\n",__FUNCTION__,__LINE__, InBytesRead));
                continue;
            }

            // Check sequence validity.
            if((int)(ntohl(UsrpHeader->SequenceNum) - InData->SequenceNum) <= 0) {
                LOG_NORM(("%s#%d: USRP Packet out of sequence (Is %u, Expecting > %u), size %i\n",__FUNCTION__,__LINE__, UsrpHeader->SequenceNum, InData->SequenceNum, InBytesRead));
                continue;
            }

            InData->SequenceNum = ntohl(UsrpHeader->SequenceNum);

            // TODO: Mutex.
            InData->Header = *UsrpHeader;

            switch(UsrpHeader->Type)
            {
            case USRP_TYPE_VOICE:
                {
                    const size_t CopySize = USRP_VOICE_FRAME_SIZE * sizeof(short);

                    InData->Write(UsrpVoiceFrame, CopySize);
                    write(AudioC->Socket, UsrpVoiceFrame, CopySize);
                }
                break;

            case USRP_TYPE_DTMF:
                LOG_NORM(("%s#%d: USRP_TYPE_DTMF unimplemented %i bytes\n",__FUNCTION__,__LINE__, InBytesRead));
                break;

            case USRP_TYPE_TEXT:
                LOG_NORM(("%s#%d: USRP_TYPE_TEXT unimplemented %i bytes\n",__FUNCTION__,__LINE__, InBytesRead));
                break;
            }
        }
       LOG_NORM(("%s#%d: RecvMain InBytesRead %i\n",__FUNCTION__,__LINE__, InBytesRead));
    } while (InBytesRead > 0 && bRunning && !bShutdown);

    LOG_NORM(("%s#%d: RecvMain exit\n",__FUNCTION__,__LINE__));
    return NULL;
}
