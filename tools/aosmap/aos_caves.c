// ace of spades map generator
// CAVES
//   version 2
//     public domain Sean Barrett May 2011

#include "stb_aos.inl"

float green_terrain_bias = 0.5;

void build_ground(void)
{
   int water_squares_desired = stb_rand() % ((MAP_X * MAP_Y) / 64);
   float water_height;
   float wmin, wmax;
   int i,j,k,n;
   int center_x = MAP_X*3/4 + (int) (stb_frand() * 16 - 8);


   // a count of water squares: the same density as the 512 map had
   water_squares_desired = 2048 * ((MAP_X * MAP_Y) / (512 * 512));

   grid_random(temp,  4,  4, 0,24, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64);
   grid_random(data,  4,  4, 0,12, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, data, 1);
   grid_random(data, 16, 16, 0,12, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, data, 1);
   grid_random(data, 32, 32, 0, 8, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, data, 1);
   memcpy(data, temp, sizeof(data));

   grid_minimize(data);
   grid_limit(data, 32);

   for (i=0; i < MAP_X; ++i) {
      // compute weighting for green vs. blue style terrain, but
      // make sure it wraps around at edges
      int greenish[17] = { 4,2,0,0, 0,0,2,4, 7,8,9,9, 9,9,8,6, 4 };
      int x;
      float top, bottom;
      // compute distance from center_x, wrapped around
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
         data[i][j] = stb_linear_remap(data[i][j], 0, 24, bottom, top);
      }
   }

   compute_water_height(water_squares_desired);

   // now add some randomness to the shores
   grid_random(temp, 64, 64, -1,1,0, 0,0,stb_frand()*64);
   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         // if data is in the range 0..4 then apply this with strength 1..0
         float w_raw = stb_linear_remap(data[i][j], 0, 4, 1, 0);
         float w = smoothstep(w_raw);
         w = stb_lerp(w, 0.25, 1);
         data[i][j] += w * temp[i][j];
      }
   }

   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         int z;
         // now ramp up at the middle of the map
         int p = abs(i - MAP_X/2);
         float t;
         //t = stb_linear_remap(p, 85, 170, 1, 0);
         t = stb_linear_remap(p, 85-45,170-62, 1, 0);

         //if (t > 0.95) t = stb_linear_remap(t,0.95,1.2,0.95,1.0);
         t = smooth_cubestep(t);//* smooth_squarestep(t);
         data[i][j] += 64 * t;
         if (data[i][j] > 63.5)
            data[i][j] = 63.5;
      }
   }

   build_height_array();

   add_lighting_detail();

   compute_ground_lighting(4,24);
}

// reuse the pillar logic from skybridge to create caves connection
// various cave points
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

   h = (stb_rand() % 40) + 10;
   if (x < MAP_X/4 || x >= MAP_X*3/4)
      if (stb_rand() % 10 < 7)
         h = stb_rand() % 20 + 30;

   pillar[num_pillars].type = type;
   pillar[num_pillars].x = x;
   pillar[num_pillars].y = y;
   pillar[num_pillars].h = h;
   pillar[num_pillars].c[0] = 0;
   pillar[num_pillars].c[1] = 0;
   pillar[num_pillars].c[2] = 0;
   ++num_pillars;
}

uint8 nearby_pillar[MAX_PILLARS][MAX_PILLARS];

void connect_pillar(int i, int j)
{
   int rad = 2 + (stb_rand() % 10 < 7);
   int bottom = rad == 2 ? -2 : -1;

   int x = (pillar[i].x + pillar[j].x)/2 + (stb_rand() % 15) - 7;
   int y = (pillar[i].y + pillar[j].y)/2 + (stb_rand() % 15) - 7;
   int z = (pillar[i].h + pillar[j].h)/2;

   draw_partial_sphere_line(pillar[i].x, pillar[i].y, pillar[i].h,
                            x,y,z,
                            rad,bottom,2,
                            pillar[i].c, pillar[j].c, 0);

   draw_partial_sphere_line(x,y,z,
                            pillar[j].x, pillar[j].y, pillar[j].h,
                            rad,bottom,2,
                            pillar[i].c, pillar[j].c, 0);
}

#define NUM_PASSAGES 10
void make_skyways(void)
{
   int i,j,k;
   for (i=0; i < num_pillars; ++i) {
      int best_for_dir[NUM_PASSAGES], best_distance[NUM_PASSAGES]; // determine the best pillars in 8 directions
      int x = pillar[i].x;
      int y = pillar[i].y;
      int h = pillar[i].h;
      int n;

      for (j=0; j < NUM_PASSAGES; ++j) {
         best_for_dir[j] = -1;
         // connections reach a third of the map, as on the 512 map
         best_distance[j] = MAP_XS(160)*MAP_XS(160);
      }

      for (j=0; j < num_pillars; ++j) {
         int dx = pillar[j].x - x;
         int dy = pillar[j].y - y;
         int dz = abs(pillar[j].h - h);
         int d, e;
         if (i == j) continue;

         // z distance is special -- we weight AGAINST connecting them if
         // z is too close, but it's ok if the z is far
         d = dx*dx + dy*dy + dz*dz*dz;

         // we keep the best_distance array sorted
         for (k=0; k < NUM_PASSAGES; ++k)
            if (d < best_distance[k])
               break;
         if (k < NUM_PASSAGES) {
            memmove(best_for_dir+k+1, best_for_dir+k, sizeof(best_for_dir[0]) * (NUM_PASSAGES-k-1));
            memmove(best_distance+k+1, best_distance+k, sizeof(best_distance[0]) * (NUM_PASSAGES-k-1));
            best_for_dir[k] = j;
            best_distance[k] = d;
         }
      }

      // now that we've found the best distances, go ahead and build a connection,
      // if we haven't already
      n = NUM_PASSAGES - (stb_rand() % 4);
      for (k=0; k < n; ++k) {
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

void draw_pillar(int x, int y, int z, uint32 color, int stairs)
{
   int r;
   int r2;
   int i,j,k;

   ++z;

   draw_rotated_ellipse(x,y,z+2, stb_frand()*M_PI, stb_frand()*4+3, stb_frand()*4+3, stb_frand()*3+2, 0);
   if (height[x][y] > z && x >= MAP_X/4 - MAP_X/16 && x <= MAP_X*3/4 + MAP_X/16)
      draw_column(x,y,z-2,z+4, 0x80ffffff);
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

   for (i=tile; i+tile < MAP_X; i += tile) {
      for (j=tile; j+tile < MAP_Y; j += tile) {
         int x = i + stb_rand() % (tile - 12) + 4;
         int y = j + stb_rand() % (tile - 12) + 6;
         make_pillar(x,y,0);
      }
   }

   // now place an equal number more pillars, but never too close to an existing pillar
   // (a count of pillars: the same density as the 512 map had)
   for (n=0; n < 200 * ((MAP_X * MAP_Y) / (512 * 512)); ++n) {
      i = stb_rand() % (MAP_X-16) + 8;
      j = stb_rand() % (MAP_Y-16) + 8;
      for (k=0; k < num_pillars; ++k)
         if (abs(pillar[k].x - i) < 22 && abs(pillar[k].y - j) < 14)
            break;
      if (k == num_pillars) {
         make_pillar(i,j,1);
      }
   }

   // now connect nearby caves
   make_skyways();

   for (n=0; n < num_pillars; ++n) {
      uint32 c = (pillar[n].c[0]<<16) + (pillar[n].c[1] << 8) + (pillar[n].c[2]) + 0x80000000;
      //assert(pillar[n].c[0] == 255 || pillar[n].c[1] == 255 || pillar[n].c[2] == 255);
      i = pillar[n].x;
      j = pillar[n].y;
      k = pillar[n].h;
      draw_pillar(i,j,k, c, pillar[n].type == 0);
   }

   // now recolor the caverns
   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {   
         int h = height[i][j]-3;
         for (k=0; k < h; ++k) {
            // the following is slow, so only compute it for surface pixels
            if (is_surface(i,j,k)) {
               float n = stb_perlin_noise3(i/8.0, j/8.0, k/6.0, 1,0,0);
               float m = stb_perlin_noise3(i/13.0, j/13.0, k/12.0, 2,0,0);
               float d = stb_perlin_noise3(i/5.0, j/5.0, k/5.0, 3,0,0);
               float t = sin(k/4.0 + d*3)/2+1;
               float r0,r1,g0,g1,b0,b1;
               int r,g,b;
               r0 = stb_lerp(n, 60, 100);
               g0 = stb_lerp(n, 60,  70);
               b0 = stb_lerp(n, 60,  50);

               r1 = stb_lerp(m, 80, 100);
               g1 = stb_lerp(m, 80, 120);
               b1 = stb_lerp(m, 80, 130);
               r = stb_lerp(t,r0,r1);
               g = stb_lerp(t,g0,g1);
               b = stb_lerp(t,b0,b1);
               color[i][j][k] = 0x80000000 + (r<<16) + (g << 8) + (b);
            }
         }
      }
   }

   // draw the top color so when connecting it's not blank
   for (i=MAP_X/2 - MAP_XS(30); i < MAP_X/2 + MAP_XS(30); ++i)
      for (j=0; j < MAP_Y; ++j)
         color[i][j][62] = i & 1 ? 0x80c0c0a0 : 0x80c8b8a0;

   build_debug();
}

