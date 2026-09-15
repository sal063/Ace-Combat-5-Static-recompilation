#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/file.h>
#include <fcntl.h>
#endif

#define MC_ENTRIES 256
#define MC_BYTES (8u*1024u*1024u)
#define MC_HANDLES 32
typedef struct { char path[256]; u8 info[64]; u8 *data; u32 size; } mc_entry;
typedef struct {
    mc_entry e[MC_ENTRIES];
    int loaded, failed, formatted, dirty, cursor;
    char cwd[256], pattern[256], file[1024];
} mc_card;
static mc_card cards[2];
static struct { int used, card, entry; u32 pos, mode; } handles[MC_HANDLES];
static const char magic[8] = "AC5MC01";
static u16 rd16(const u8 *p) { return p[0] | ((u16)p[1]<<8); }
static void wr16(u8 *p, u16 n) { p[0]=(u8)n; p[1]=(u8)(n>>8); }
static void wr32(u8 *p, u32 n) { for(int i=0;i<4;i++) p[i]=(u8)(n>>(8*i)); }
static u32 rd32(const u8 *p) { return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24); }
static int directory(const mc_entry *e) { return (rd16(e->info+20)&32)!=0; }
static void stamp(u8 *p) {
    time_t now=time(NULL); struct tm *t=localtime(&now);
    memset(p,0,8); if(!t) return;
    p[1]=t->tm_sec; p[2]=t->tm_min; p[3]=t->tm_hour;
    p[4]=t->tm_mday; p[5]=t->tm_mon+1; wr16(p+6,(u16)(t->tm_year+1900));
}
static int find(mc_card *c,const char *path) {
    for(int i=0;i<MC_ENTRIES;i++) if(c->e[i].path[0]&&!strcmp(c->e[i].path,path)) return i;
    return -1;
}
static u32 used_clusters(mc_card *c) {
    u32 n=16;
    for(int i=0;i<MC_ENTRIES;i++) if(c->e[i].path[0]) n+=1+(c->e[i].size+1023)/1024;
    return n;
}
static void clear_card(mc_card *c) {
    for(int i=0;i<MC_ENTRIES;i++) { free(c->e[i].data); memset(&c->e[i],0,sizeof(c->e[i])); }
    strcpy(c->cwd,"/"); c->cursor=0; c->pattern[0]=0; c->dirty=1;
}
static void make_entry(mc_entry *e,const char *p,int dir) {
    memset(e,0,sizeof(*e)); strcpy(e->path,p);
    wr16(e->info+20,dir?0x8027:0x8097);
    stamp(e->info); memcpy(e->info+8,e->info,8);
}
static int commit(mc_card *c) {
    if(c->failed) return -5;
    if(!c->dirty) return 0;
    char temp[1060]; snprintf(temp,sizeof temp,"%s.tmp",c->file);
    FILE *f=fopen(temp,"wb"); if(!f) return -5;
    u8 h[16]={0}; memcpy(h,magic,8);
    u32 count=0; for(int i=0;i<MC_ENTRIES;i++) count+=c->e[i].path[0]!=0;
    wr32(h+8,count); wr32(h+12,c->formatted);
    int ok=fwrite(h,1,16,f)==16;
    for(int i=0;i<MC_ENTRIES&&ok;i++) if(c->e[i].path[0]) {
        mc_entry *e=&c->e[i]; wr32(e->info+16,e->size);
        ok=fwrite(e->path,1,256,f)==256 && fwrite(e->info,1,64,f)==64;
        if(ok&&e->size) ok=fwrite(e->data,1,e->size,f)==e->size;
    }
    if(fflush(f)) ok=0;
#ifdef _WIN32
    if(ok&&_commit(_fileno(f))) ok=0;
#else
    if(ok&&fsync(fileno(f))) ok=0;
#endif
    if(fclose(f)) ok=0;
#ifdef _WIN32
    if(ok&&!MoveFileExA(temp,c->file,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) ok=0;
#else
    if(ok&&rename(temp,c->file)) ok=0;
#endif
    if(!ok) { ps2_log("memcard: could not commit '%s'; previous card preserved",c->file); return -5; }
    c->dirty=0; return 0;
}
static int load_card(int port) {
    mc_card *c=&cards[port];
    if(c->loaded) return c->failed?-5:0;
    c->loaded=1; strcpy(c->cwd,"/");
    const char *root=getenv("PS2_SAVE_DIR"); if(!root||!*root) root="saves";
    if(strlen(root)>900) { c->failed=1; return -5; }
    ps2_mkdir_p(root);
    snprintf(c->file,sizeof c->file,"%s/card%d.ps2mc",root,port);
    char lockpath[1060]; snprintf(lockpath,sizeof lockpath,"%s.lock",c->file);
#ifdef _WIN32
    HANDLE lock=CreateFileA(lockpath,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(lock==INVALID_HANDLE_VALUE) { c->failed=1; return -5; }
#else
    int lock=open(lockpath,O_CREAT|O_RDWR,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)) { if(lock>=0) close(lock); c->failed=1; return -5; }
#endif
    FILE *f=fopen(c->file,"rb");
    if(!f) {
        if(errno!=ENOENT) { c->failed=1; return -5; }
        c->formatted=1; make_entry(&c->e[0],"/",1); c->dirty=1;
        if(commit(c)) { c->failed=1; return -5; }
        ps2_log("memcard: created '%s'",c->file); return 0;
    }
    u8 h[16]; int ok=fread(h,1,16,f)==16&&!memcmp(h,magic,8);
    u32 count=ok?rd32(h+8):0, total=0;
    if(!ok||count>MC_ENTRIES||rd32(h+12)>1) ok=0;
    if(ok) c->formatted=rd32(h+12);
    for(u32 i=0;i<count&&ok;i++) {
        mc_entry *e=&c->e[i];
        ok=fread(e->path,1,256,f)==256&&fread(e->info,1,64,f)==64;
        if(!ok||!memchr(e->path,0,256)||e->path[0]!='/') { ok=0; break; }
        for(u32 j=0;j<i;j++) if(!strcmp(c->e[j].path,e->path)) ok=0;
        e->size=rd32(e->info+16);
        if(e->size>MC_BYTES-total||(directory(e)&&e->size)) { ok=0; break; }
        total+=e->size;
        if(e->size) { e->data=malloc(e->size); if(!e->data||fread(e->data,1,e->size,f)!=e->size) ok=0; }
    }
    if(fgetc(f)!=EOF||ferror(f)) ok=0;
    fclose(f);
    int rootidx=find(c,"/");
    if(c->formatted&&(rootidx<0||!directory(&c->e[rootidx]))) ok=0;
    if(!ok||used_clusters(c)>8192) {
        clear_card(c); c->failed=1;
        ps2_log("memcard: invalid card '%s'; refusing to overwrite it",c->file); return -5;
    }
    ps2_log("memcard: loaded '%s'",c->file); return 0;
}
static int ram_ok(u32 a,u32 n) { a&=0x1fffffff; return a<PS2_RAM_SIZE&&n<=PS2_RAM_SIZE-a; }
static void to_guest(u32 a,const void *data,u32 n) { const u8 *p=data; for(u32 i=0;i<n;i++) ps2_w8(a+i,p[i]); }
static void from_guest(void *data,u32 a,u32 n) { u8 *p=data; for(u32 i=0;i<n;i++) p[i]=ps2_r8(a+i); }
static int path_name(mc_card *c,u32 a,char *out) {
    char in[1024],joined[1280]; unsigned i;
    for(i=0;i<sizeof(in)-1;i++) { in[i]=(char)ps2_r8(a+i); if(!in[i]) break; }
    if(i==sizeof(in)-1) return -5;
    snprintf(joined,sizeof joined,"%s%s%s",in[0]=='/'?"":c->cwd,in[0]=='/'?"":"/",in);
    strcpy(out,"/"); char *p=joined;
    while(*p) {
        while(*p=='/'||*p=='\\') p++;
        if(!*p) break;
        char part[33]; unsigned n=0;
        while(*p&&*p!='/'&&*p!='\\') { if(n==32) return -5; part[n++]=*p++; }
        part[n]=0;
        if(!strcmp(part,".")) continue;
        if(!strcmp(part,"..")) { char *q=strrchr(out,'/'); if(q!=out) *q=0; else out[1]=0; continue; }
        if(strchr(part,':')||strlen(out)+n+2>=256) return -5;
        if(strcmp(out,"/")) strcat(out,"/");
        strcat(out,part);
    }
    return 0;
}
static int parent(mc_card *c,const char *path) {
    char p[256]; strcpy(p,path); char *q=strrchr(p,'/'); if(q==p) p[1]=0; else *q=0;
    int i=find(c,p); return i>=0&&directory(&c->e[i]);
}
static int child(const char *dir,const char *p) {
    size_t n=strlen(dir); return strcmp(dir,"/")? !strncmp(dir,p,n)&&p[n]=='/' : p[0]=='/'&&p[1];
}
static int match(const char *pat,const char *s) {
    const char *star=NULL,*retry=NULL;
    while(*s) {
        if(*pat=='?'||*pat==*s) { pat++;s++; }
        else if(*pat=='*') { star=pat++;retry=s; }
        else if(star) { pat=star+1;s=++retry; }
        else return 0;
    }
    while(*pat=='*') pat++;
    return !*pat;
}
static int entry_open(int port,int e) {
    for(int i=0;i<MC_HANDLES;i++) if(handles[i].used&&handles[i].card==port&&handles[i].entry==e) return 1;
    return 0;
}
static int operation(u32 f,u32 s,int ssize) {
    int named=f==2||f==12||f==13||f==14||f==15||f==18;
    if(ssize<(named?1044:48)||!ram_ok(s,(u32)ssize)) return -5;
    int port=(int)ps2_r32(s+(named?0:4)), slot=(int)ps2_r32(s+(named?4:8));
    u32 fd=ps2_r32(s);
    if(f==3||f==4||f==5||f==6||f==10) {
        if(fd>=MC_HANDLES||!handles[fd].used) return -5;
        port=handles[fd].card; slot=0;
    }
    if(f==20) return 0;
    if(f==21) return port>=0&&port<2?1:-5;
    if(port<0||port>1||slot!=0) return -1;
    int rc=load_card(port); if(rc) return rc;
    mc_card *c=&cards[port];
    if(f==1) {
        u32 a=ps2_r32(s+28); if(!ram_ok(a,192)) return -5;
        u8 info[192]={0}; wr32(info,2); wr32(info+4,8192-used_clusters(c)); wr32(info+144,c->formatted);
        to_guest(a,info,sizeof info); return c->formatted?0:-2;
    }
    if(f==16||f==17) {
        for(int i=0;i<MC_HANDLES;i++) if(handles[i].used&&handles[i].card==port) return -5;
        clear_card(c); c->formatted=f==16;
        if(c->formatted) make_entry(&c->e[0],"/",1);
        return commit(c);
    }
    if(!c->formatted) return -2;
    if(f==3||f==10) { rc=commit(c); if(!rc&&f==3) handles[fd].used=0; return rc; }
    if(f==4||f==5||f==6) {
        mc_entry *e=&c->e[handles[fd].entry];
        u32 pos=handles[fd].pos, n=ps2_r32(s+12), buf=ps2_r32(s+24);
        if(f==4) {
            u32 whence=ps2_r32(s+20); if(whence>2) return -5;
            s64 p=(s32)ps2_r32(s+16)+(s64)(whence==0?0:whence==1?pos:e->size);
            if(p<0||p>MC_BYTES) return -5;
            handles[fd].pos=(u32)p; return (int)p;
        }
        if(f==5) {
            if(!(handles[fd].mode&1)||n>MC_BYTES||!ram_ok(buf,n)) return -5;
            u32 fix=ps2_r32(s+28); if(!ram_ok(fix,192)) return -5;
            u8 zero[192]={0}; to_guest(fix,zero,192);
            if(pos>=e->size) n=0; else if(n>e->size-pos) n=e->size-pos;
            if(n) to_guest(buf,e->data+pos,n);
            handles[fd].pos=pos+n; return n;
        }
        u32 lead=ps2_r32(s+20);
        if(!(handles[fd].mode&2)||lead>16||n>MC_BYTES-lead||(n&&!ram_ok(buf,n))) return -5;
        if(handles[fd].mode&0x100) pos=e->size;
        u32 total=n+lead; if(total>MC_BYTES-pos) return -3;
        u32 end=pos+total;
        if(end>e->size) {
            if(used_clusters(c)-(e->size+1023)/1024+(end+1023)/1024>8192) return -3;
            u8 *p=realloc(e->data,end); if(!p) return -3;
            memset(p+e->size,0,end-e->size); e->data=p; e->size=end;
        }
        if(lead) from_guest(e->data+pos,s+32,lead);
        if(n) from_guest(e->data+pos+lead,buf,n);
        handles[fd].pos=end; stamp(e->info+8); c->dirty=1; return total;
    }
    if(!named) return -5;
    char path[256]; if(path_name(c,s+20,path)) return -5;
    int idx=find(c,path); u32 flags=ps2_r32(s+8), dest=ps2_r32(s+16);
    if(f==2) {
        if(strchr(path,'*')||strchr(path,'?')) return -5;
        if(flags&64) {
            if(idx>=0) return -5;
            if(!parent(c,path)) return -4;
            if(used_clusters(c)>=8192) return -3;
            for(idx=0;idx<MC_ENTRIES&&c->e[idx].path[0];idx++);
            if(idx==MC_ENTRIES) return -3;
            make_entry(&c->e[idx],path,1); c->dirty=1; return commit(c);
        }
        if(!(flags&3)||((flags&0x400)&&!(flags&2))) return -5;
        int h; for(h=0;h<MC_HANDLES&&handles[h].used;h++);
        if(h==MC_HANDLES) return -7;
        if(idx<0) {
            if(!(flags&0x200)) return -4;
            if(!parent(c,path)) return -4;
            if(used_clusters(c)>=8192) return -3;
            for(idx=0;idx<MC_ENTRIES&&c->e[idx].path[0];idx++);
            if(idx==MC_ENTRIES) return -3;
            make_entry(&c->e[idx],path,0); c->dirty=1;
        }
        mc_entry *e=&c->e[idx];
        if(directory(e)||entry_open(port,idx)) return -5;
        if(((flags&1)&&!(rd16(e->info+20)&1))||((flags&2)&&!(rd16(e->info+20)&2))) return -5;
        if(flags&0x400) { free(e->data); e->data=NULL; e->size=0; c->dirty=1; }
        handles[h].used=1; handles[h].card=port; handles[h].entry=idx;
        handles[h].mode=flags; handles[h].pos=(flags&0x100)?e->size:0; return h;
    }
    if(f==13) {
        s32 max=(s32)ps2_r32(s+12); if(max<-1||max>MC_ENTRIES+2) return -5;
        if(!flags) { strcpy(c->pattern,path); c->cursor=0; }
        else if(strcmp(c->pattern,path)) return -5;
        if(max>0&&!ram_ok(dest,(u32)max*64)) return -5;
        char dir[256]; strcpy(dir,path); *strrchr(dir,'/')=0;
        if(!dir[0]) strcpy(dir,"/");
        const char *pattern=strrchr(path,'/')+1;
        int parentidx=find(c,dir); if(parentidx<0||!directory(&c->e[parentidx])) return -4;
        int found=0, start=max==-1?0:c->cursor;
        for(int i=start;i<MC_ENTRIES+2;i++) {
            if(max>=0&&found==max) { c->cursor=i; return found; }
            mc_entry *item;
            const char *name;
            if(i<2) {
                name=i?"..":".";
                char p[256]; strcpy(p,dir);
                if(i&&strcmp(p,"/")) { char *q=strrchr(p,'/'); if(q==p) p[1]=0; else *q=0; }
                int pi=find(c,p); if(pi<0) continue;
                item=&c->e[pi];
            } else {
                item=&c->e[i-2];
                if(!item->path[0]||!strcmp(item->path,"/")) continue;
                name=strrchr(item->path,'/')+1;
                size_t plen=(size_t)(name-item->path-1);
                if(strcmp(dir,"/")?(strlen(dir)!=plen||strncmp(dir,item->path,plen)):plen!=0) continue;
            }
            if(!match(pattern,name)) continue;
            if(max>=0) { u8 row[64]; memcpy(row,item->info,64); wr32(row+16,item->size); memset(row+32,0,32); memcpy(row+32,name,strlen(name)); to_guest(dest+found*64,row,64); }
            found++;
        }
        if(max>=0) c->cursor=MC_ENTRIES+2;
        return found;
    }
    if(idx<0) return -4;
    mc_entry *e=&c->e[idx];
    if(f==12) {
        if(!directory(e)||!ram_ok(dest,1024)) return -5;
        u8 old[1024]={0}; strcpy((char*)old,c->cwd); to_guest(dest,old,1024); strcpy(c->cwd,path); return 0;
    }
    if(f==18) {
        if(!directory(e)) return -4;
        int free_entries=0;
        for(int i=0;i<MC_ENTRIES;i++) free_entries+=!c->e[i].path[0];
        return free_entries;
    }
    if(f==15) {
        if(!strcmp(path,"/")||entry_open(port,idx)) return -5;
        for(int i=0;i<MC_ENTRIES;i++) if(c->e[i].path[0]&&child(path,c->e[i].path)) return -6;
        free(e->data); memset(e,0,sizeof(*e)); c->dirty=1; return commit(c);
    }
    if(f==14) {
        if(!ram_ok(dest,64)) return -5;
        u8 info[64]; from_guest(info,dest,64);
        if(flags&16) {
            if(!strcmp(path,"/")||entry_open(port,idx)||!memchr(info+32,0,32)) return -5;
            const char *name=(const char*)info+32;
            if(!*name||strpbrk(name,"/\\:*?")||!strcmp(name,".")||!strcmp(name,"..")) return -5;
            char renamed[256]; strcpy(renamed,path); *(strrchr(renamed,'/')+1)=0;
            if(strlen(renamed)+strlen(name)>=256) return -5;
            strcat(renamed,name);
            if(find(c,renamed)>=0) return -5;
            size_t oldlen=strlen(path),newlen=strlen(renamed);
            for(int i=0;i<MC_ENTRIES;i++) if(c->e[i].path[0]&&child(path,c->e[i].path)) {
                if(entry_open(port,i)||newlen+strlen(c->e[i].path+oldlen)>=256) return -5;
            }
            for(int i=0;i<MC_ENTRIES;i++) if(c->e[i].path[0]&&child(path,c->e[i].path)) {
                char p[256]; strcpy(p,renamed); strcat(p,c->e[i].path+oldlen); strcpy(c->e[i].path,p);
            }
            if(!strcmp(c->cwd,path)||child(path,c->cwd)) { char p[256]; snprintf(p,sizeof p,"%s%s",renamed,c->cwd+oldlen); strcpy(c->cwd,p); }
            strcpy(e->path,renamed);
        }
        if(flags&1) memcpy(e->info,info,8);
        if(flags&2) memcpy(e->info+8,info+8,8);
        if(flags&4) wr16(e->info+20,(rd16(info+20)&~0x30u)|(rd16(e->info+20)&0x30u)|0x8000);
        c->dirty=1; return commit(c);
    }
    return -5;
}
int ps2_memcard_rpc(ps2_ctx *ctx,u32 f,u32 send,int ssize,u32 recv,int rsize) {
    (void)ctx;
    if(!recv||rsize<4||!ram_ok(recv,(u32)rsize)) return 0;
    for(int i=0;i<rsize;i++) ps2_w8(recv+i,0);
    if(f==254) {
        if(rsize>=12) { ps2_w32(recv+4,522); ps2_w32(recv+8,526); }
    } else ps2_w32(recv,(u32)operation(f,send,ssize));
    return 0;
}
