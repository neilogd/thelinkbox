#ifndef _USRP_H_
#define _USRP_H_

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

enum
{
    USRP_TYPE_VOICE = 0,
    USRP_TYPE_DTMF,
    USRP_TYPE_TEXT,

    USRP_VOICE_FRAME_SIZE = 160, // 20ms
    USRP_VOICE_FRAMES_MAX = 10,  // 200ms

    USRP_VOICE_BUFFER_SIZE = USRP_VOICE_FRAME_SIZE * USRP_VOICE_FRAMES_MAX,
};


struct USRPHeader
{
	char Usrp[4];	        // "USRP"
	uint32_t SequenceNum;
	uint32_t Memory;
	uint32_t Ptt;
	uint32_t Talkgroup;
	uint32_t Type;
	uint32_t Mpxid;
	uint32_t Reserved;
};


class CUSRP
{
public:
    CUSRP();
    ~CUSRP();

    int Init(char *AudioDevice);

private:
	static void* StaticSendMain( void* pParam );
	static void* StaticRecvMain( void* pParam );
	void* SendMain();
	void* RecvMain();
    
    pthread_t SendThread;
    pthread_t RecvThread;

    struct sockaddr_in InServerAddr;
    struct sockaddr_in InClientAddr;
    int InSock;
    socklen_t InAddrLen;

    uint32_t InSequenceNum;
    short InAudioBuffer[USRP_VOICE_BUFFER_SIZE];
    int InAudioBufferWriteOff;
    int InAudioBufferReadOff;
};


#endif // _USRP_H_