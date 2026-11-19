// ace of spades map generator
// Forest_River
//   version 1
//     public domain Sean Barrett May 2011

#include "stb_aos.inl"


uint8 water[MAP_X][MAP_Y];

void make_water(void)
{
   int i,j,k;
   // reset water value
   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         water[i][j] = (water[i][j] ? 0 : 255);
      }
   }
   // measure distance to water
   for (k=0; k < 64; ++k) {
      for (i=0; i < MAP_X; ++i) {
         for (j=0; j < MAP_Y; ++j) {
            if (water[i][j] == k) {
               int x0 = (i-1) & MASK_X;
               int x1 = (i+1) & MASK_X;
               int y0 = (j-1) & MASK_Y;
               int y1 = (j+1) & MASK_Y;
               if (water[i][y0] == 255) water[i][y0] = k+1;
               if (water[i][y1] == 255) water[i][y1] = k+1;
               if (water[x0][j] == 255) water[x0][j] = k+1;
               if (water[x1][j] == 255) water[x1][j] = k+1;
            }
         }
      }
   }
   // lower ground around water to match water
   for (i=0; i < MAP_X; ++i) {
      float clamp, reach;
      int fake_i = i;
      if (i < MAP_X/16) fake_i = stb_linear_remap(i, 0,MAP_X/16, MAP_X/16,MAP_X);
      clamp = stb_linear_remap(fake_i, MAP_X/16,MAP_X, 1.0, 1.1);
      reach = stb_linear_remap(fake_i, MAP_X/16,MAP_X, MAP_XS(12),MAP_XS(24));
      for (j=0; j < MAP_Y; ++j) {
         if (water[i][j] < 255) {
            float t = stb_linear_remap(water[i][j], 0, reach, 0, 1);
            if (t < 1) {
               t = stb_clamp(t,0,clamp);
               t = pow(t,0.7);
               data[i][j] *= t;
            }
         }
      }
   }
}

void draw_pool(int x, int y, int r)
{
   int r2 = r*r + (2*r+1)/2;
   int i,j;
   for (i = -r; i <= r; ++i) {
      for (j = -r; j <= r; ++j) {
         if (i*i + j*j <= r2) {
            int a = (x+i) & MASK_X;
            int b = (y+j) & MASK_Y;
            water[a][b] = 1;
         }
      }
   }
}

void draw_river_segment(int x0, int y0, int x1, int y1, int r)
{
   int n = stb_max(abs(x1-x0), abs(y1-y0)) + stb_rand() % 5;
   int i;
   for (i=0; i <= n; ++i) {
      float t = stb_linear_remap(i,0,n, 0.0,1.0);
      draw_pool(stb_lerp(t,x0,x1)+stb_rand()%3-1, stb_lerp(t,y0,y1)+stb_rand()%3-1, r);
   }
}

void draw_recursive_river(int x0, int y0, int x1, int y1, int r, int s)
{
   int x = (x0 + x1) / 2;
   int y = (y0 + y1) / 2;
   int dx = abs(x - x0);
   int dy = abs(y - y0);
   int dist = stb_max(dx,dy) + stb_min(dx,dy)/2;
   int displace = dist / 8;
   if (displace > 1) {
      x = x + (stb_rand() % (displace*2+1)) - displace;
      y = y + (stb_rand() % (displace*2+1)) - displace;
   }
   if (dist < 4) {
      draw_river_segment(x0,y0,x0,y0,r);
      draw_river_segment(x,y,x1,y1,r);
   } else {
      draw_recursive_river(x0,y0,x,y,r,s+1);
      if (s == 0) {
         int off;
         draw_recursive_river(x,y,x1,y1,r-1,s+1);
         off = stb_rand() % 24 + 24;
         if (stb_rand() & 8)
            off = -off;
         draw_recursive_river(x,y,x1,y1 + off,r-1,s+1);
      } else
         draw_recursive_river(x,y,x1,y1,r,s+1);
   }
}

void draw_random_river(int x, int y, int dx, int dy, int r)
{
   int y2 = stb_linear_remap(MAP_X, x, x+dx*MAP_XS(100), y,y+dy*MAP_XS(150));
   draw_recursive_river(x, y, MAP_X, y2, r, 0);
}

void build_ground(void)
{
   float water_height;
   float wmin, wmax;
   float s;
   int i,j,k,n, x,y;
   int center_x = MAP_X*3/4 + (int) (stb_frand() * 16 - 8);
   int last_vertical = 0;
   int rx,ry;


   // generate the "forest" data into 'data'
   grid_random(temp,  2,  2, 0, 20, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64);
   grid_random(data,  4,  4, 0, 20, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, data, 1);
   grid_random(data,  8,  8, 0, 16, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, data, 1);
   grid_random(data, 16, 16, 0, 14, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, data, 1);

   memcpy(data, temp, sizeof(data));

   // generate the "river" terrain into 'temp'
   grid_random(temp,  8, 8, 0, 10, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64);
   grid_random(temp2, 16, 16, 0, 10, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, temp2, 1);
   grid_random(temp2, 32, 32, 0, 8, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, temp2, 1);

   grid_minimize(data);
   grid_minimize(temp);

   for (i=0; i < MAP_X; ++i) {
      // compute weighting for green vs. blue style terrain, but
      // make sure it wraps around at edges
      int greenish[17] = { 2,1,0,0, 0,0,2,4, 5,7,9,9, 9,9,8,7, 2 };
      int x;
      float top, bottom;
      // compute distance from center_x, wrapped around
      int g = (i * 16) / MAP_X;
      int g0 = greenish[g];
      int g1 = greenish[g+1];
      float off = (i * 16) / (float) MAP_X - g;
      float t = stb_lerp(off, g0, g1) / 9.0f;
      // now t is 0 in blue and 1 in green

      for (j=0; j < MAP_Y; ++j) {
         int z;
         data[i][j] = stb_lerp(t, data[i][j], temp[i][j]) + 1.5;
      }
   }

   s = stb_frand() * 64;

   // choose "fork" location for river
   x = MAP_X*3/4 + (stb_rand() % (MAP_X/8) - MAP_X/8);
   y = MAP_Y/2 + (stb_rand() % MAP_YS(150) - MAP_YS(75));

   // draw "ocean" -- @TODO slightly larger and push to wraparound to blue a little
   for (i=0; i < MAP_XS(31); ++i) {
      int ty = MAP_Y/2 + (y-MAP_Y/2)/2;
      int half = MAP_XS(31)/2;
      int range = abs(i-half);
      range = sqrt(half*half - range*range);
      // 12 is the inlet's own y/x ratio (180 y to 31 x), not a map length:
      // half scales with the map, the ratio does not
      range *= 12;
      for (j=-range; j <= range; ++j) {
         water[MAP_X-1-i][(ty+j) & MASK_Y] = 1;
      }
   }

   rx = x;
   ry = y;

   // draw river delta
   for (i=0; i < 9; i += 2)
      draw_random_river(x,y, 4,i-4, 2 + stb_rand() % 3);
   
   // draw river into blue territory
   while (x > 0) {
      int dx = -MAP_XS(24) + (stb_rand() % MAP_XS(12));
      int dy;

      if (x > MAP_X/2)
         dy = stb_rand() % MAP_XS(24) - MAP_XS(12);
      else
         dy = stb_rand() % MAP_XS(40) - MAP_XS(20);
      if (y < MAP_Y/8)
         dy += MAP_XS(20);
      else if (y < MAP_Y/4)
         dy += MAP_XS(10);
      else if (y >= MAP_Y - MAP_Y/8)
         dy -= MAP_XS(20);
      else if (y >= MAP_Y - MAP_Y/4)
         dy -= MAP_XS(10);

      if (abs(dy) > MAP_XS(12) && !last_vertical && x < MAP_X/2 + MAP_X/8) {
         last_vertical = 1;
         dx = dx/2;
      } else
         last_vertical = 0;

      if (x + dx < 0) dx = -x;

      draw_river_segment(x,y, x+dx, y+dy, 3);
      x += dx;
      y += dy;
   }
   draw_river_segment(MAP_X-1,y, rx,ry, 2);

   // create riverbed to follow water
   make_water();

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

   compute_ground_lighting(4, 24);
}

void place_trees(void)
{
   int i,j;
   for (i=6; i+7 < MAP_X; i += 7) {
      float density = stb_linear_remap(i,0,MAP_X, 1.5, -0.5);
      density = smoothstep(density) * 0.75;
      for (j=6; j+7 < MAP_Y; j += 7) {
         if (stb_frand() < density) {
            // place something
            int x = i + stb_rand() % 4;
            int y = j + stb_rand() % 4;
            float dx = data[x][y] - data[x+1][y];
            float dy = data[x][y] - data[x][y-1];
            if (dx*dx+dy*dy < 0.35 && height[x][y] > 1 && height[x][y] < 63-4) {
               int z = height[x][y] + 1;
               if (stb_frand() < density * 0.75 && z+7 < 64) {
                  uint32 c = 0x80507040;
                  // trunk
                  draw_column(x,y,z,z+3, 0x80604020);
                  // crown
                  if (stb_rand() & 4)
                     z += 2;
                  else
                     z += 3;

                  draw_column(x-2,y-1,z,z, c);
                  draw_column(x-2,y+0,z,z, c);
                  draw_column(x-2,y+1,z,z, c);

                  draw_column(x+2,y-1,z,z, c);
                  draw_column(x+2,y+0,z,z, c);
                  draw_column(x+2,y+1,z,z, c);

                  draw_column(x-1,y-2,z,z, c);
                  draw_column(x+0,y-2,z,z, c);
                  draw_column(x+1,y-2,z,z, c);

                  draw_column(x-1,y+2,z,z, c);
                  draw_column(x+0,y+2,z,z, c);
                  draw_column(x+1,y+2,z,z, c);

                  draw_column(x-1,y-1,z,z+1, c);
                  draw_column(x-1,y+0,z,z+2, c);
                  draw_column(x-1,y+1,z,z+1, c);

                  draw_column(x+1,y-1,z,z+1, c);
                  draw_column(x+1,y+0,z,z+2, c);
                  draw_column(x+1,y+1,z,z+1, c);

                  draw_column(x-1,y-1,z,z+1, c);
                  draw_column(x+0,y-1,z,z+2, c);
                  draw_column(x+1,y-1,z,z+1, c);

                  draw_column(x-1,y+1,z,z+1, c);
                  draw_column(x+0,y+1,z,z+2, c);
                  draw_column(x+1,y+1,z,z+1, c);

                  draw_column(x,y,z,z+3, c);
               } else if (stb_frand() < density * 0.75) {
                  uint32 c = 0x80206020;
                  draw_column(x,y,z,z+1+stb_rand()%2, c);
                  draw_column(x+1,y,z,z+1+stb_rand()%2, c);
                  draw_column(x,y+1,z,z+1+stb_rand()%2, c);
                  draw_column(x+1,y+1,z,z+1+stb_rand()%2, c);
               } else {
                  uint32 c = 0x80204020;
                  draw_column(x,y,z,z+1+stb_rand()%2, c);
               }
            }
         }
      }
   }
}

void build_map(char *opts[256])
{
   int tile;
   int n,i,j,k;

   build_ground();

   // -n: no trees, the bare valley (the mountains and the river alone)
   if (!opts['n'])
      place_trees();

   build_debug();
}

