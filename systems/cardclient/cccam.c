/*
 * Softcam plugin to VDR (C++)
 *
 * This code is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This code is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 * Or, point your browser to http://www.gnu.org/copyleft/gpl.html
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include <vdr/pat.h>

#include "cc.h"
#include "network.h"
//#include "parse.h"

#define LIST_ONLY 0x03   /* CA application should clear the list when an 'ONLY' CAPMT object is received, and start working with the object */

static char *socketPath="/var/emu/chroot%d/tmp/camd.socket";

// -- cCCcamCard ---------------------------------------------------------------

class cCCcamCard : public cMutex {
private:
  int ccam_fd;
  int cardnum, pid, pmtlen;
  bool newcw;
  cTimeMs timecw;
  unsigned char *capmt, cw[16];
  const char *path;
  cCondVar cwwait;
public:
  cCCcamCard(void);
  ~cCCcamCard();
  void Setup(int num, const char *Path);
  bool Connect(void);
  void Disconnect(void);
  bool Connected(void) { return ccam_fd>=0; }
  void WriteCaPmt(void);
  void NewCaPmt(int p, const unsigned char *pmt, int len);
  int Pid(void) { return pid; }
  bool GetCw(unsigned char *Cw, int timeout);
  void NewCw(const unsigned char *Cw);
  };

cCCcamCard::cCCcamCard(void)
{
  cardnum=-1; path=0;
  ccam_fd=-1; capmt=0; newcw=false; pid=-1;
}

cCCcamCard::~cCCcamCard()
{
  Disconnect();
  free(capmt);
}

void cCCcamCard::Setup(int num, const char *Path)
{
  cardnum=num; path=Path;
}

void cCCcamCard::Disconnect(void)
{
  cMutexLock lock(this);
  close(ccam_fd);
  ccam_fd=-1; newcw=false;
}

bool cCCcamCard::Connect(void)
{
  cMutexLock lock(this);
  Disconnect();
  ccam_fd=socket(AF_LOCAL,SOCK_STREAM,0);
  if(!ccam_fd) {
    PRINTF(L_CC_CCCAM,"%d: socket failed: %s",cardnum,strerror(errno));
    return false;
    }
  sockaddr_un serv_addr_un;
  memset(&serv_addr_un,0,sizeof(serv_addr_un));
  serv_addr_un.sun_family=AF_LOCAL;
  snprintf(serv_addr_un.sun_path,sizeof(serv_addr_un.sun_path),path,cardnum);
  if(connect(ccam_fd,(const sockaddr*)&serv_addr_un,sizeof(serv_addr_un))!=0) {
    PRINTF(L_CC_CCCAM,"%d: connect failed: %s",cardnum,strerror(errno));
    Disconnect();
    return false;
    }
  PRINTF(L_CC_CCCAM,"%d: opened camd socket",cardnum);
  return true;
}

void cCCcamCard::WriteCaPmt(void)
{
  cMutexLock lock(this);
  if(capmt) {
    for(int retry=2; retry>0; retry--) {
      if(!Connected() && !Connect()) break;
      int r=write(ccam_fd,capmt,pmtlen);
      if(r==pmtlen) {
        newcw=false;
        break;
        }
      PRINTF(L_CC_CCCAM,"%d: write failed: %s",cardnum,strerror(errno));
      Disconnect();
      }
    }
}

void cCCcamCard::NewCaPmt(int p, const unsigned char *pmt, int len)
{
  cMutexLock lock(this);
  free(capmt); pid=0; newcw=false;
  capmt=MALLOC(unsigned char,len);
  if(capmt) {
    memcpy(capmt,pmt,len);
    pmtlen=len;
    capmt[6]=LIST_ONLY;
    pid=p;
    WriteCaPmt();
    }
}

void cCCcamCard::NewCw(const unsigned char *Cw)
{
  cMutexLock lock(this);
  if(memcmp(cw,Cw,sizeof(cw))) {
    memcpy(cw,Cw,sizeof(cw));
    newcw=true;
    timecw.Set();
    cwwait.Broadcast();
    }
}

bool cCCcamCard::GetCw(unsigned char *Cw, int timeout)
{
  cMutexLock lock(this);
  if(newcw && timecw.Elapsed()>3000)
    newcw=false; // too old
  if(!newcw)
    cwwait.TimedWait(*this,timeout);
  if(newcw) {
    memcpy(Cw,cw,sizeof(cw));
    newcw=false;
    return true;
    }
  return false;
}

// -- cCardClientCCcam ---------------------------------------------------------

class cCardClientCCcam : public cCardClient , private cThread {
private:
  cNetSocket so;
  cCCcamCard card[4];
  int pmtversion;
  int failedcw;
protected:
  virtual bool Login(void);
  virtual void Action(void);
public:
  cCardClientCCcam(const char *Name);
  virtual bool Init(const char *CfgDir);
  virtual bool ProcessECM(const cEcmInfo *ecm, const unsigned char *data, unsigned char *Cw, int cardnum);
  virtual bool CanHandle(unsigned short SysId);
  };

static cCardClientLinkReg<cCardClientCCcam> __ncd("CCcam");

cCardClientCCcam::cCardClientCCcam(const char *Name)
:cCardClient(Name)
,cThread("CCcam listener")
,so(DEFAULT_CONNECT_TIMEOUT,2,3600,true)
{
  pmtversion=0;
  for(int i=0; i<4; i++) card[i].Setup(i,socketPath);
}

bool cCardClientCCcam::Init(const char *config)
{
  cMutexLock lock(this);
  int num=0;
  if(!ParseStdConfig(config,&num)) return false;
  return true;
}

bool cCardClientCCcam::CanHandle(unsigned short SysId)
{
  if((SysId & 0xf000) != 0x5000) return true;
  return false;
}

bool cCardClientCCcam::Login(void)
{
  if(!so.Connected()) {
    so.Disconnect();
    if(!so.Bind("127.0.0.1",port)) return false;
    PRINTF(L_CC_CCCAM,"Bound to port %d, starting UDP listener",port);
    Start();
    }
  return true;
}

bool cCardClientCCcam::ProcessECM(const cEcmInfo *ecm, const unsigned char *data, unsigned char *cw, int cardnum)
{
  // if(((ccam_fd ==0) && !Login()) || !CanHandle(ecm->caId)) return false;
  //so.Flush();
  //  cMutexLock lock(this);
  //newcw[cardnum] =0;

  static const unsigned char pmt[] = {
    0x9f,0x80,0x32,0x82,0x00,0x00,
    0x01,
#define PRG_POS 7
    0xFF,0xFF,                                          // prg-nr
#define VERS_POS 9
    0xFF,                                               // version
#define LEN_POS 10
    0xFF,0xFF,                                          // prg-info-len
    0x01,                                               // ca pmt command
#define PRIV_POS 13
    0x81,0x08,0x00,0x00,0xFF,0x00,0xFF,0xFF,0xFF,0xFF,  // private descr
#define DMX_POS 23
    0x82,0x02,0xFF,0xFF,                                // demuxer stuff
#define PID_POS 27
    0x84,0x02,0xFF,0xFF                                 // pmt pid
    };
  unsigned char capmt[2048];
  memcpy(capmt,pmt,sizeof(pmt));
  int wp=sizeof(pmt);
  int len=wp-LEN_POS-2;
  capmt[PRG_POS]=ecm->prgId>>8;
  capmt[PRG_POS+1]=ecm->prgId&0xff;
  capmt[VERS_POS]=pmtversion;
  pmtversion=(pmtversion+1)&0x1f;
  capmt[PRIV_POS+4]=cardnum;
  capmt[PRIV_POS+6]=ecm->transponder>>8;
  capmt[PRIV_POS+7]=ecm->transponder&0xFF;
  capmt[PRIV_POS+8]=ecm->provId>>8;
  capmt[PRIV_POS+9]=ecm->provId&0xFF;
  capmt[DMX_POS+2]=1<<cardnum ;
  capmt[DMX_POS+3]=cardnum ;
  capmt[PID_POS+2]=ecm->ecm_pid>>8;
  capmt[PID_POS+3]=ecm->ecm_pid&0xFF;
  bool streamflag = 1;
#if APIVERSNUM >= 10500
  int casys[2];
#else
  unsigned short casys[2];
#endif
  casys[0]=ecm->caId;
  casys[1]=0;
  int n=GetCaDescriptors(ecm->source,ecm->transponder,ecm->prgId,casys,sizeof(capmt)-wp,&capmt[wp],streamflag);
  if(n<=0) {
    PRINTF(L_CC_CCCAM,"no CA descriptor for caid %04x sid %d prov %04x",ecm->caId,ecm->prgId,ecm->provId);
    return false;
    }
  len+=n; wp+=n;
  capmt[wp++]=0x01;
  capmt[wp++]=0x0f;
  capmt[wp++]=cardnum;   // cccam uses this one as PID to program ca0
  capmt[wp++]=0x00;      //es_length
  capmt[wp++]=0x06;      //es ca_pmt_cmd_id
  capmt[LEN_POS]=((len&0xf00)>>8);
  capmt[LEN_POS+1]=(len&0xff);
  capmt[4]=(wp-6)>>8;
  capmt[5]=(wp-6)&0xff;

  cCCcamCard *c=&card[cardnum];
  int timeout=700;
  if(ecm->ecm_pid!=c->Pid() || !c->Connected()) { // channel change
    PRINTF(L_CC_CCCAM,"sending capmts ");
    c->NewCaPmt(ecm->ecm_pid,capmt,wp);
    timeout=3000;
    }
  if(!c->GetCw(cw,timeout)) {
    // somethings up, so we will send capmt again.
    c->WriteCaPmt();
    timeout=1000;
    if(!c->GetCw(cw,timeout)) {
      PRINTF(L_CC_CCCAM,"%d: FAILED ECM !",cardnum);
      c->Disconnect();
      failedcw++;
      if(failedcw>=10) {
        // CCcam is having problems lets mark it for a restart....
        FILE *f=fopen("/tmp/killCCcam","w+");
        fclose(f);
        failedcw=0;
        }
      return false;
      }
    }
  PRINTF(L_CC_CCCAM,"%d: GOT CW !",cardnum);
  failedcw=0;
  return true;
}

void cCardClientCCcam::Action()
{
  unsigned char cw[18];
  while(Running()) {
    if(so.Read(cw,sizeof(cw))==sizeof(cw)) {
      PRINTF(L_CC_CCCAM," Got: %02hhx%02hhx  %02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx  %02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
        cw[0],cw[1],
        cw[2],cw[3],cw[4],cw[5],cw[6],cw[7],cw[8],cw[9],
        cw[10],cw[11],cw[12],cw[13],cw[14],cw[15],cw[16],cw[17]);

      if(cw[1]==0x0f && cw[0]<4)
        card[cw[0]].NewCw(cw+2);
      }
    }
}