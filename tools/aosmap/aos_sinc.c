// ace of spades map generator
// SINC
//   version 1
//     public domain Sean Barrett May 2011

#include "stb_aos.inl"


// given a number from 0..1, make sure it wraps around smoothly
float match_edges(float x)
{
   if (x < 1.0 / 16) return stb_linear_remap(x, 0, 1.0/16, 0.5, 1.0/16);
   if (x >= 15.0/16) return stb_linear_remap(x, 15.0/16, 1, 15.0/16, 0.5);
   return x;
}

void build_ground(void)
{
   int i,j,k,n;

   float low00,low01,low10,low11;
   float high00,high01,high10,high11;
   float lh00, lh01, lh10, lh11;
   float hh00, hh01, hh10, hh11;

   float rate = stb_lerp(stb_frand(), 28,44);
   float spread = stb_lerp(stb_frand(), 0.6, 0.9);
   
   high00 = stb_lerp(stb_frand(), 11,24);
   high01 = stb_lerp(stb_frand(),  9,18);
   high10 = stb_lerp(stb_frand(), 11,24);
   high11 = stb_lerp(stb_frand(),  9,18);

   hh00 = stb_lerp(stb_frand(), 8, 16);
   hh01 = stb_lerp(stb_frand(), 8, 13);
   hh10 = stb_lerp(stb_frand(), 8, 16);
   hh11 = stb_lerp(stb_frand(), 8, 13);

   low00 = stb_lerp(stb_frand(),  2, 6);
   low01 = stb_lerp(stb_frand(), -1, 4);
   low10 = stb_lerp(stb_frand(),  2, 6);
   low11 = stb_lerp(stb_frand(), -1, 4);

   lh00 = stb_lerp(stb_frand(), 7, 11);
   lh01 = stb_lerp(stb_frand(), 5, 11);
   lh10 = stb_lerp(stb_frand(), 7, 11);
   lh11 = stb_lerp(stb_frand(), 5, 11);

   for (i=0; i < MAP_X; ++i) {
      float u = match_edges(stb_linear_remap(i,0,MAP_X,0.0,1.0));
      float h0 = stb_lerp(u,high00,high01);
      float h1 = stb_lerp(u,high10,high11);
      float l0 = stb_lerp(u,low00, low01);
      float l1 = stb_lerp(u,low10, low11);
      float dh0 = stb_lerp(u,hh00,hh01);
      float dh1 = stb_lerp(u,hh10,hh11);
      float dl0 = stb_lerp(u,lh00,lh01);
      float dl1 = stb_lerp(u,lh10,lh11);
      for (j=0; j < MAP_Y; ++j) {
         float v = match_edges(stb_linear_remap(j,0,MAP_Y,0.0,1.0));
         float fh = stb_lerp(v,h0,h1);
         float fl = stb_lerp(v,l0,l1);
         float fdh = stb_lerp(v,dh0,dh1);
         float fdl = stb_lerp(v,dl0,dl1);
         int bi = (i + MAP_X/2) & MASK_X;
         int bj = (j + MAP_Y/2) & MASK_Y;
         float x = (bi - (MAP_X/2-0.5)) / (float) (MAP_X/2);
         float y = (bj - (MAP_X/2-0.5)) / (float) (MAP_X/2);
         float r = sqrt(x*x + y*y); // 0 .. 1.4
         //float bottom = stb_lerp(r, 16,2);
         //float top = stb_lerp(r, 26,10);
         float bottom = stb_lerp(r, fh,fl);
         float top = stb_lerp(r, fh+fdh,fl+fdl);
         float ang = pow(r,spread)*rate;
         float h = stb_lerp(cos(ang)/2+0.5, bottom, top);
         data[i][j] = h;
      }
   }

   // now add some randomness to the shores

   add_shore_detail();

   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         int z = (int) data[i][j];
         if (z < 0) z = 0;
         height[i][j] = z;
         for (k=0; k < MAP_Z; ++k)
            map[i][j][k] = (k <= z);
      }
   }

   // add some lighting detail
   add_lighting_detail();

   compute_ground_lighting(4,24);
}


void build_map(char *opts[256])
{
   build_ground();

   build_debug();
}
