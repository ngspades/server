// ace of spades map generator
// SKYBRIDGE
//   version 2
//     public domain Sean Barrett May 2011

#include "stb_aos.inl"

float green_terrain_bias = 0.25;
float green_skybridge_bias = 1;

void build_ground(void)
{
   int water_squares_desired;
   int i,j;

   grid_random(temp, 4, 4, 0,24,0, 256*stb_frand(), 256*stb_frand(),64*stb_frand());
   grid_random(data, 2, 2, 0, 16, 0, 256*stb_frand(), 256*stb_frand(),64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data, 4, 4, 0, 12, 0, 256*stb_frand(), 256*stb_frand(),64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data, 8, 8, 0, 16, 0, 256*stb_frand(), 256*stb_frand(),64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data,16,16, -6, 6, 0, 256*stb_frand(), 256*stb_frand(),64*stb_frand()); grid_addeq_scale(temp, data, 1);

   memcpy(data, temp, sizeof(data));

   for (i=0; i < MAP_X; ++i) {
      // compute weighting for green vs. blue style terrain, but
      // make sure it wraps around at edges
      int greenish[17] = { 4,2,0,0, 0,0,2,4, 7,8,9,9, 9,9,8,6, 4 };
      int x;
      float top, bottom;
      int g = (i * 16) / MAP_X;
      int g0 = greenish[g];
      int g1 = greenish[g+1];
      float off = (i * 16) / (float) MAP_X - g;
      float t = stb_lerp(off, g0, g1) / 9.0f;
      // now t is 0 in blue and 1 in green

      // warp this so green is lower
      top    = stb_lerp(t, 16 + 16*green_terrain_bias, 16);
      bottom = stb_lerp(t, -8 + 16*green_terrain_bias, -8);

      for (j=0; j < MAP_Y; ++j) {
         int z;
         data[i][j] = (data[i][j] * data[i][j])/16.0f; // square to get steeper tops
         data[i][j] = stb_linear_remap(data[i][j], 0, 12, bottom, top);
      }
   }
   grid_limit(data, 32);

   water_squares_desired = stb_rand() % ((MAP_X * MAP_Y) / 100);
   compute_water_height(water_squares_desired);

   add_shore_detail();

   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         // if data is in the range 0..4 then apply this with strength 1..0
         float w_raw = stb_linear_remap(data[i][j], 0, 4, 1, 0);
         float w = smoothstep(w_raw);
         w = stb_lerp(w, 0.5, 1);
         data[i][j] += w * temp[i][j];
      }
   }

   build_height_array();

   add_lighting_detail();

   compute_ground_lighting(4,24);
}

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
   float s,t, hw;
   int h,z;

   // compute idealized elevation for pillar
   int b = height[x][y];

   t = stb_linear_remap(abs(MAP_X/2 - x), 0, MAP_X/2, 1, -0.125);
   t = stb_clamp(t,0,1);
   //t = sqrt(t);
   // t is 1 at the center of the map and 0 at the edges

   s = stb_linear_remap(x, 0, MAP_X, 1, 0);
   // s is 1 in the blue map and 0 in the green

   if (type == 0)
      hw = t;
   else
      hw = stb_lerp(green_skybridge_bias, t, s);

   // z is the lowest the pillar height should be if it's "tall"
   z = b + 24;
   if (z > 56) z = 56;
   // z is a random tall pillar height
   z = z + stb_rand() % (56-z);

   h = stb_lerp(hw, b, z);
   if (h > 56) h = 56;

   pillar[num_pillars].type = type;

   pillar[num_pillars].x = x;
   pillar[num_pillars].y = y;
   pillar[num_pillars].h = h;
   pillar[num_pillars].c[0] = stb_rand();
   pillar[num_pillars].c[1] = stb_rand();
   pillar[num_pillars].c[2] = stb_rand();
   pillar[num_pillars].c[stb_rand() % 3] = 255;
   ++num_pillars;
}

uint8 nearby_pillar[MAX_PILLARS][MAX_PILLARS];

void connect_pillar(int i, int j)
{
   draw_partial_sphere_line(pillar[i].x, pillar[i].y, pillar[i].h,
                            pillar[j].x, pillar[j].y, pillar[j].h,
                            2,-1,1,
                            pillar[i].c, pillar[j].c, 0x80);
}

void make_skyways(void)
{
   int i,j,k;

   for (i=0; i < num_pillars; ++i) {
      int best_for_dir[8], best_distance[8]; // determine the best pillars in 8 directions
      int x = pillar[i].x;
      int y = pillar[i].y;
      int h = pillar[i].h;

      for (j=0; j < 8; ++j) {
         best_for_dir[j] = -1;
         best_distance[j] = (MAP_X/8)*(MAP_X/8);   // we never make connections that are more than 1/4 the map length
      }

      // we always allow routes that go left-to-right
      best_distance[0] = best_distance[4] = (MAP_X/4)*(MAP_X/4);

      // we deemphasize routes that go right-to-left
      best_distance[2] = best_distance[6] = (MAP_X*3/32)*(MAP_X*3/32);

      for (j=0; j < num_pillars; ++j) {
         int dx = pillar[j].x - x;
         int dy = pillar[j].y - y;
         int dz;
         int dir, d;
         if (i == j) continue;
         // determine the direction
         //              x  2  x
         //        3     x     x     1
         //               x   x
         //    xxxx       x   x       xxxx
         //        xxxx    x x    xxxx
         //            xxxxx xxxxx
         //     4          xxx          0
         //            xxxxx xxxxx
         //        xxxx    x x    xxxx
         //    xxxx       x   x       xxxx
         //               x   x
         //         5    x     x    7
         //              x  6  x

         if (abs(dx)*3/4 > abs(dy)) {
            dir = dx > 0 ? 0 : 4;
         } else if (abs(dy)*3/4 > abs(dx)) {
            dir = dy > 0 ? 2 : 6;
         } else if (dx > 0) {
            dir = dy > 0 ? 1 : 7;
         } else
            dir = dy > 0 ? 3 : 5;

         assert(dir >= 0 && dir < 8);

         dz = abs(pillar[j].h - h);
         // z distance is special -- we weight AGAINST connecting them if the
         // z is too close, but it's ok if the z is far
         d = dx*dx + dy*dy + 0*64*dz; // when distance is ~64 z stops weighting as much

         if (d < best_distance[dir]) {
            best_for_dir[dir] = j;
            best_distance[dir] = d;
         }
      }

      #if 0
      // if near edge of map, discard forward directions
      if (x >= MAP_X/4 && x <= MAP_X*3/4 && (y < MAP_YS(100) || y >= MAP_Y - MAP_YS(100))) {
         if (stb_rand() & 15 < 11)
            best_for_dir[0] = -1;
         if (stb_rand() & 15 < 11)
            best_for_dir[4] = -1;
      }
      #endif

      #if 1
      // now check if any proposed directions are too similar
      for (k=0; k < 8; ++k) {
         int d = best_for_dir[k];
         int e = best_for_dir[(k+1) & 7];
         if (d >= 0 && e >= 0) {
            // if the directions are too similar:
            float ddir[2] = { pillar[d].x - x, pillar[d].y - y };
            float edir[2] = { pillar[e].x - x, pillar[e].y - y };
            float sc;
            sc = 1.0 / sqrt(ddir[0]*ddir[0] + ddir[1]*ddir[1]);
            ddir[0] *= sc;
            ddir[1] *= sc;
            sc = 1.0 / sqrt(edir[0]*edir[0] + edir[1]*edir[1]);
            edir[0] *= sc;
            edir[1] *= sc;
            if (edir[0]*ddir[0] + edir[1]*ddir[1] > 0.75) {
               // check if either one is ALREADY built
               int w;

               assert(d >= 0 && d < MAX_PILLARS);
               assert(e >= 0 && e < MAX_PILLARS);
               assert(i >= 0 && i < MAX_PILLARS);
               if (nearby_pillar[i][d])
                  w = (k+1)&7;
               else if (nearby_pillar[i][e])
                  w = k;
               else
                  w = k|1; // choose the odd one (the diagonal)
               // remove this one
               assert(w >= 0 && w < 8);
               best_for_dir[w] = -1;
            }
         }
      }
      #endif

      // now that we've found the best distances, go ahead and build a connection,
      // if we haven't already
      for (k=0; k < 8; ++k) {
         if (best_for_dir[k] >= 0) {
            j = best_for_dir[k];
            if (!nearby_pillar[i][j]) {
               connect_pillar(i,j);
               nearby_pillar[i][j] = 1;
               nearby_pillar[j][i] = 1;
            }
         }
      }
   }   
}

void draw_stair(int x, int y, int z, int d, int w, uint32 color)
{
   int clear_top = 7;
   --z;
   while (z > height[x][y]) {
      int e;
      for (e=-w; e <= w; ++e) {
         int h = height[x][y+e];
         if (h < z-6) h = z-6;
         draw_column(x,y+e, h+1, z, color);
      }
      draw_column(x,y,z+1, z+clear_top, 0);
      --clear_top;
      if (clear_top < 4) clear_top = 4;
      z -= 2;
      x += d;
      if (x >= MAP_X || x < 0) return;
   }
}

void draw_pillar(int x, int y, int z, uint32 color, int stairs)
{
   int r;
   int r2;
   int i,j,k;

   ++z;

   // draw the main support
   r = 2;
   r2 = 6;
   for (i=-r; i <= r; ++i) {
      for (j=-r; j <= r; ++j) {
         if (i*i + j*j <= r2) {
            k = height[x+i][y+j]+1;
            draw_column(x+i,y+j, k,z,color);   
         }
      }
   }

   // draw the platform
   draw_sphere_slice(x,y,z, 4, 0x80404040, -2,0);

   // draw a little thing to hide behind
   draw_column(x,y,z+1, z+4, 0x80ffffff);

   r = 4;

   if (stairs) {
      if (x < MAP_X/4)
         draw_stair(x-r,y,z,-1,1, color);
      else if (x < MAP_X/2+MAP_X/8)
         draw_stair(x-r,y,z,-1,0, color);

      if (x >= MAP_X*3/4)
         draw_stair(x+r,y,z, 1,1, color);
      else if (x >= MAP_X/2-MAP_X/8)
         draw_stair(x+r,y,z, 1,0, color);
   }
}

void build_map(char *opts[256])
{
   int tile;
   int n,i,j,k;

   build_ground();

   // now build the sky bridges
   //
   // start by placing one pillar in every grid tile, jittered

   tile = MAP_X / 8;

   for (i=0; i < MAP_X; i += tile) {
      for (j=0; j < MAP_Y; j += tile) {
         int x = i + stb_rand() % (tile - 12) + 4;
         int y = j + stb_rand() % (tile - 12) + 6;
         // don't place pillars in "edge" tiles at center
         // to discourage long runs that way
         if (i >= MAP_X/2 - tile*2 && i < MAP_X/2 + tile*2)
            if (j < tile || j >= MAP_Y-tile)
               continue;
         if (i >= MAP_X/2 - tile && i < MAP_X/2 + tile)
            if (j < tile*2 || j >= MAP_Y-tile*2)
               continue;
         make_pillar(x,y,0);
      }
   }

   // now place an equal number more pillars, but never too close to an existing pillar
   // (a count of pillars: the same density as the 512 map had)
   for (n=0; n < 64 * ((MAP_X * MAP_Y) / (512 * 512)); ++n) {
      i = stb_rand() % (MAP_X-16) + 8;
      j = stb_rand() % (MAP_Y-16) + 8;
      // don't place pillars in "edge" tiles at center
      // to discourage long runs that way
      if (i >= MAP_X/2 - tile*2 && i < MAP_X/2 + tile*2)
         if (j < tile || j >= MAP_Y-tile)
            continue;
      if (i >= MAP_X/2 - tile && i < MAP_X/2 + tile)
         if (j < tile*2 || j >= MAP_Y-tile*2)
            continue;
      for (k=0; k < num_pillars; ++k)
         if (abs(pillar[k].x - i) < 22 && abs(pillar[k].y - j) < 14)
            break;
      if (k == num_pillars) {
         make_pillar(i,j,1);
      }
   }

   // now, for each pillar, find the other pillar along each of 8 directions
   make_skyways();

   for (n=0; n < num_pillars; ++n) {
      uint32 c = (pillar[n].c[0]<<16) + (pillar[n].c[1] << 8) + (pillar[n].c[2]) + 0x80000000;
      //assert(pillar[n].c[0] == 255 || pillar[n].c[1] == 255 || pillar[n].c[2] == 255);
      i = pillar[n].x;
      j = pillar[n].y;
      k = pillar[n].h;
      draw_pillar(i,j,k, c, pillar[n].type == 0);
   }

   build_debug();
}

