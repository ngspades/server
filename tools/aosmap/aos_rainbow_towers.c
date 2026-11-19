// ace of spades map generator
// RAINBOW TOWERS
//   version 3
//     public domain Sean Barrett May 2011

#include "stb_aos.inl"   // http://nothings.org/stb.h

void build_ground(void)
{
   float water_height;
   int i,j,k,n;

   grid_random(temp,  2,  2, 0,  6,  0 , 256*stb_frand(), 256*stb_frand(), 64*stb_frand());
   grid_random(data,  4,  4, 0, 16,  0 , 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data,  8,  8, 0, 20,  0 , 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data, 16, 16, 0, 12,  0 , 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data, 32, 32, 0,  8, 0.5, 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data, 64, 64, 0,  3,  0 , 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, data, 1);

   memcpy(data, temp, sizeof(data));

   grid_minimize(data);
   grid_limit(data, 36);

   water_height = stb_lerp(stb_frand(),-1,4);
   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         data[i][j] = data[i][j] - water_height;
      }
   }

   // now add some randomness to the shores
   add_shore_detail();

   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         // if data is in the range 0..4 then apply this with strength 1..0
         float w_raw = stb_linear_remap(data[i][j], 0, 4, 1, 0);
         float w = smoothstep(w_raw);
         w = stb_lerp(w, 0.25, 1);
         data[i][j] += w * temp[i][j];
      }
   }

   build_height_array();

   add_lighting_detail();

   compute_ground_lighting(4, 24);
}

// pillars are now caves
typedef struct
{
   int x, y, h;
   uint8 c[3];
   uint8 type;
} pillar_data;

#define MAX_PILLARS 1024

pillar_data pillar[MAX_PILLARS];
int num_pillars;

void make_pillar(int x, int y, int type)
{
   unsigned char colors[7][3] =
   {
      { 255,0,0, },
      { 255,165,0 },
      { 255,255,0 },
      { 0,192,0 },
      { 64,64,255 },
      { 120,40,255 },
      { 200, 0,255 },
   };

   float s,t, hw;
   int h,z;

   h = height[x][y] + 32;
   if (h > 60) h = 60;

   pillar[num_pillars].type = type;
   pillar[num_pillars].x = x;
   pillar[num_pillars].y = y;
   pillar[num_pillars].h = h;
   pillar[num_pillars].c[0] = colors[num_pillars][0];
   pillar[num_pillars].c[1] = colors[num_pillars][1];
   pillar[num_pillars].c[2] = colors[num_pillars][2];
   ++num_pillars;
}

uint32 light_color(uint32 color, float brightness)
{
   int r = (uint8) (color >> 16);
   int g = (uint8) (color >>  8);
   int b = (uint8) (color >>  0);

   r *= brightness;
   g *= brightness;
   b *= brightness;

   return (color & 0xff000000) + r*65536 + g*256 + b;
}

void draw_pillar(int x, int y, int z, uint32 col)
{
   int r;
   int r2;
   int i,j,k;

   ++z;

   // draw a larger base to make it harder to destroy, and if
   // they destroy up higher to make it faster, it still leaves
   // a nice high platform
   draw_sphere(x,y,height[x][y], 12, col);

   // draw the main support
   r = 6;
   r2 = (36+49)/2;
   for (i=-r; i <= r; ++i) {
      for (j=-r; j <= r; ++j) {
         if (i*i + j*j <= r2) {
            k = height[x+i][y+j]+1;
            draw_column(x+i,y+j, k,z,col);   
         }
      }
   }

   // draw the platform
   draw_sphere_slice(x,y,z, 9, col, -3,0);

   // draw a little thing to hide behind
   //draw_column(x,y,z+1, z+4, 0x80ffffff);
   draw_sphere_slice(x,y,z+1, 8, col,  0,0);
   draw_sphere_slice(x,y,z+1, 7,   0,  0,0);

   for (i=-9; i <= 9; ++i) {
      for (j=-9; j <= 9; ++j) {
         float bright1 = stb_linear_remap(i,-9,9, 0.5,1);
         float bright2 = sqrt(i*i+j*j);
         bright1 = stb_clamp(bright1, 0.5, 1);
         bright2 = stb_linear_remap(bright2, 0, 9, 0.5, 1);
         bright2 = stb_clamp(bright2, 0.5, 1);
         color[x+i][y+j][z] = light_color(col, stb_clamp(bright1*bright2,0.35,1));
      }
   }         
}

void build_map(char *opts[256])
{
   int tile;
   int n,i,j,k;

   build_ground();

   // now place an equal number more pillars, but never too close to an existing pillar
   // a count of towers (and the sampling budget for placing them): the same
   // density as the 512 map had, so the spacing below stays 100 blocks and a
   // bigger map holds more of them
   for (n=0; n < 5000 * ((MAP_X * MAP_Y) / (512 * 512)) &&
               num_pillars < 7 * ((MAP_X * MAP_Y) / (512 * 512)); ++n) {
      // pillars can fall in green's territory, but not blue's
      i = (int) stb_lerp(stb_frand(), MAP_X/4+MAP_X/16, MAP_X*3/4+MAP_X/16);
      // pillars are more likely to be near the center vertically
      if ((stb_rand() % 100) < ((i < MAP_X/2-MAP_X/8 || i > MAP_X/2+MAP_X/8) ? 90u : 70u))
         j = rnd(MAP_Y/4+MAP_Y*3/32, MAP_Y*3/4-MAP_Y*3/32);
      else
         j = rnd(MAP_Y/4-MAP_Y/16, MAP_Y*3/4+MAP_Y/16);
      for (k=0; k < num_pillars; ++k) {
         int dx = pillar[k].x - i;
         int dy = pillar[k].y - j;
         if (dx * dx + dy * dy < 100*100)
            break;
      }
      if (k == num_pillars)
         make_pillar(i,j,0);
   }

   for (n=0; n < num_pillars; ++n) {
      uint32 c = (pillar[n].c[0]<<16) + (pillar[n].c[1] << 8) + (pillar[n].c[2]) + 0x80000000;
      //assert(pillar[n].c[0] == 255 || pillar[n].c[1] == 255 || pillar[n].c[2] == 255);
      i = pillar[n].x;
      j = pillar[n].y;
      k = pillar[n].h;
      draw_pillar(i,j,k, c);
   }

   if (debug) {
      for (k=0; k < MAP_Z; ++k) {
         for (i=0; i < MAP_Y/2; ++i) {
            color[MAP_X/4][i+MAP_Y/4][k] = 0x806060ff;
            color[MAP_X*3/4][i+MAP_Y/4][k] = 0x8030d030;
         }
         for (i=MAP_X/32; i < MAP_X/4; ++i) {
            color[i][MAP_Y/4][k] = 0x806060ff;
            color[i][MAP_Y*3/4][k] = 0x806060ff;
            color[MAP_X-1-i][MAP_Y/4][k] = 0x8030d030;
            color[MAP_X-1-i][MAP_Y*3/4][k] = 0x8030d030;
         }
      }
   }
}

