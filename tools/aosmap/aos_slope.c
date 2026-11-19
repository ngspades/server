// ace of spades map generator
// SLOPE
//   version 2 (tweaked amount of water)
//     public domain Sean Barrett May 2011

#include "stb_aos.inl"


void build_ground(void)
{
   int water_squares_desired = stb_lerp(pow(stb_frand(), 1.5), 1.0/64, 1.0/16) * (MAP_X * MAP_Y);
   float water_height;
   float wmin, wmax;

   float weight = stb_lerp(stb_frand(), 0.125, 1);
   int i,j,k,n;
   int center_x = MAP_X*3/4 + (int) (stb_frand() * 16 - 8);

   grid_random(temp, 4, 4,   -  4,   4, 0, 256*stb_frand(), 256*stb_frand(), 64*stb_frand());
   grid_random(data, 2, 2,   -  4,   4, 0, 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data, 4, 4,   -  8,   8, 1, 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data,  8, 8,  - 10,  10, 1, 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data, 16, 16, -  6,   6, 0, 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, data, 1);
   grid_random(data, 32, 32, -1.5, 1.5, 1, 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, data, 1);

   memcpy(data, temp, sizeof(data));

   for (i=0; i < MAP_X; ++i) {
      // compute weighting for green vs. blue style terrain, but
      // make sure it wraps around at edges
      int greenish[17] = { 3,1,0,0, 0,0,1,3, 5,7,8,8, 8,8,7,5, 3 };
      int x;
      float top, bottom, t;
      // compute distance from center_x, wrapped around
      int g = (i * 16) / MAP_X;
      int g0 = greenish[g];
      int g1 = greenish[g+1];
      float off = i / (float) MAP_X;
      if (off < 1.0/16 || off > 15.0/16) {
         if (off < 1.0/16)
            off = stb_linear_remap(off, 0,1.0/16, 0.5,0);
         else
            off = stb_linear_remap(off, 15.0/16,1.0, 1,0.5);
         off = smoothstep(off);
      } else {
         off = stb_linear_remap(off, 1.0/16, 15.0/16, 0,1);
      }

      weight = abs(i - MAP_X/2);
      weight = stb_linear_remap(weight, 0, MAP_X/2, 1, 0.75);
      
      t = off;
      // now t is 0 in blue and 1 in green
      for (j=0; j < MAP_Y; ++j) {
         data[i][j] = data[i][j]*weight + (1-t)*36 - 4;
      }
   }

   grid_limit(data, 45);

   compute_water_height(water_squares_desired);

   grid_limit(data, 45);

   // now add some randomness to the shores
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

   compute_ground_lighting(4,48);
}

void build_map(char *opts[256])
{
   int tile;
   int n,i,j,k;

   build_ground();

   build_debug();
}
