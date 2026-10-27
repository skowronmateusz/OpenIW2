#include "game/fog.h"
#include <cstdio>
using namespace iw2::game;
int main(){ FogState f; int bad=0; if(f.consts(0).active) ++bad; f.command(500,4000,0.5f,0.25f,1,0,1000); auto c=f.consts(1000); if(!c.active||std::abs(c.scale+1.0f/3500)>1e-9f||std::abs(c.shift-4000.0f/3500)>1e-6f||std::abs(c.color[0]-128/255.0f)>1e-6f) ++bad; f.command(0,2000,0,0,0,1000,2000); auto m=f.current(2500); if(std::abs(m.farDist-3000)>1e-3f) ++bad; if(f.current(3000).farDist!=2000) ++bad; if(bad) std::printf("FAIL %d\n",bad); return bad; }
