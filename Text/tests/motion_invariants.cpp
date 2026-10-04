// SPDX-License-Identifier: MIT
#include "engine.hpp"
#include <iostream>
using namespace sunimo;
static void require(bool ok,const char*message,int id){if(!ok)throw std::runtime_error(std::string(message)+" preset="+std::to_string(id));}
static double distance(const Pose&a,const Pose&b){return std::abs(a.x-b.x)+std::abs(a.y-b.y)+std::abs(a.rz-b.rz)+std::abs(a.rx-b.rx)+std::abs(a.ry-b.ry)+std::abs(a.sx-b.sx)+std::abs(a.sy-b.sy)+std::abs(a.strip-b.strip)+std::abs(a.glint-b.glint);}
int main(int argc,char**argv){try{
 if(argc!=2)return 2;auto s=Scene::file(argv[1]);Renderer r;auto&c=s->c.v;
 c[0]=120;c[1]=1.2f;c[2]=1;c[6]=0;c[7]=0;c[10]=9;c[11]=1;c[12]=1;c[14]=0;c[15]=0;
 for(int id=1;id<=100;id++)for(int mode=0;mode<=3;mode++){
  c[18]=id;c[8]=mode;r.set(s);
  for(double t:{0.,.4,1.7,3.11,6.3,22.7,59.2,117.9})for(size_t i=0;i<s->sprites.size();i++){
   auto a=r.state(i,t,120);const auto&p=a.primary;
   require(std::abs(p.x)<.07&&std::abs(p.y)<.12,"unbounded hold translation",id);
   require(std::abs(p.rz)<.08&&p.sx>.97&&p.sx<1.03&&p.sy>.97&&p.sy<1.03,"unbounded hold shape",id);
   require(distance(a.primary,r.state(i,t,120).primary)==0,"non deterministic hold",id);
   require(distance(a.body,r.state(0,t,120).body)<1e-12,"body not shared",id);
   if(mode>0&&LIFE_RECIPES[id].kind!=9)for(size_t j=0;j<i;j++){
    bool same=mode==3||(mode==1&&s->sprites[i].word==s->sprites[j].word)||(mode==2&&s->sprites[i].line==s->sprites[j].line);
    if(same)require(distance(a.primary,r.state(j,t,120).primary)<1e-12,"group broken into letters",id);
   }
  }
 }
 for(int mode=1;mode<=8;mode++){c[10]=mode;c[8]=3;r.set(s);for(size_t i=1;i<s->sprites.size();i++)require(distance(r.state(0,3,120).primary,r.state(i,3,120).primary)<1e-12,"manual life grouping broken",mode);}
 c[10]=9;c[18]=0;c[8]=0;c[7]=-1;
 for(int id=1;id<=100;id++){
  c[6]=id;r.set(s);for(double edge:{double(c[1]),119.})for(size_t i=0;i<s->sprites.size();i++){
   auto left=r.state(i,edge-1e-6,120),right=r.state(i,edge+1e-6,120);
   auto lm=r.matrix(i,left,1920,1080),rm=r.matrix(i,right,1920,1080);
   auto a=project(lm,s->sprites[i].pw*.5,s->sprites[i].ph*.5),b=project(rm,s->sprites[i].pw*.5,s->sprites[i].ph*.5);
   require(std::hypot(a[0]-b[0],a[1]-b[1])<.05,"jump at lifecycle boundary",id);
   require(std::abs(left.primary.alpha-right.primary.alpha)<.001,"alpha jump at boundary",id);
   require(std::abs(left.lifeEcho-right.lifeEcho)<.001,"echo jump at boundary",id);
  }
 }
 std::cout<<"PASS: 100 profiles, four grouping modes, bounded offsets, seek, 8 manual modes, transition boundaries\n";
 return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
