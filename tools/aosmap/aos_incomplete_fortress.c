// ace of spades map generator
// MACBETH
//   version 1
//     public domain Sean Barrett May 2011

#include "stb_aos.inl"


void draw_hstair(int x, int y, int z0, int z1, int d, int w, uint32 color)
{
   int clear_top = 8;
   int z = z1-1;
   while (z > z0) {
      int e;
      for (e=-w; e <= w; ++e) {
         int h = z-3;
         if (h < z0) h = z0;
         draw_column(x,y+e, h, z, color);
      }
      draw_column(x,y,z+1, z+clear_top, 0);
      --clear_top;
      if (clear_top < 6) clear_top = 6;
      z -= 2;
      x += d;
      if (x >= MAP_X || x < 0) return;
   }
}

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
      if (i < 32) fake_i = stb_linear_remap(i, 0,32, 32,MAP_X);
      clamp = stb_linear_remap(fake_i, 32,MAP_X, 1.0, 1.1);
      reach = stb_linear_remap(fake_i, 32,MAP_X, 12,24);
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

void build_ground(void)
{
   float water_height;
   float wmin, wmax;
   float s;
   int i,j,k,n, x,y;
   int center_x = MAP_X*3/4 + (int) (stb_frand() * 16 - 8);
   int last_vertical = 0;
   int rx,ry;

   // generate the blue terrain into data

   grid_random(temp,  2,  2, 0, 20, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64);
   grid_random(data,  4,  4, 0,  8, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, data, 1);
   grid_random(data,  8,  8, 0, 12, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, data, 1);
   grid_random(data, 16, 16, 0, 12, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, data, 1);
   grid_random(data, 32, 32, 0,  6, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, data, 1);
   memcpy(data, temp, sizeof(data));

   // generate the green terrain into temp
   grid_random(temp,   8,  8, 0, 10, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64);
   grid_random(temp2, 16, 16, 0,  6, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, temp2, 1);
   grid_random(temp2, 32, 32, 0,  4, 0, stb_frand()*256, stb_frand()*256, stb_frand()*64); grid_addeq_scale(temp, temp2, 1);

   grid_minimize(data);
   grid_minimize(temp);

   grid_limit(data, 32);

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

   // build the moat
   for (i=MAP_X*3/4; i < MAP_X; ++i)
      for (j=MAP_Y/4; j < MAP_Y*3/4; ++j)
         water[i][j] = 1;

   make_water();

   // build the interior
   #define MOAT_SIZE 6
   for (i=MAP_X*3/4 + MOAT_SIZE; i < MAP_X - MOAT_SIZE; ++i) {
      for (j=MAP_Y/4 + MOAT_SIZE; j < MAP_Y*3/4 - MOAT_SIZE; ++j) {
         int dx = (i < MAP_X*7/8) ? i-(MAP_X*3/4+MOAT_SIZE) : MAP_X-MOAT_SIZE - i;
         int dy = (j < MAP_Y/2) ? j-(MAP_Y/4+MOAT_SIZE) : MAP_Y*3/4-MOAT_SIZE - j;
         int d;
         dx = abs(dx);
         dy = abs(dy);
         d = stb_min(dx,dy);
         d = stb_linear_remap(d, 0, MAP_XS(12), 1, 4);
         d = stb_clamp(d,1,3);
         data[i][j] = d;
      }
   }

   // now add some randomness to the shores--more than normal so we don't use the shared function
   grid_random(temp,   16,  16, -1, 1, 0, 256*stb_frand(), 256*stb_frand(), 64*stb_frand());
   grid_random(temp2,  32,  32,  0, 1, 0, 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, temp2, 1);
   grid_random(temp2, 128, 128, -1, 0, 0, 256*stb_frand(), 256*stb_frand(), 64*stb_frand()); grid_addeq_scale(temp, temp2, 1);

   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         // if data is in the range 0..4 then apply this with strength 1..0
         float w_raw = stb_linear_remap(data[i][j], 0, 4, 1, 0);
         float w = smoothstep(w_raw);
         w = stb_lerp(w, 0, 3);
         data[i][j] += w * temp[i][j] + temp[i][j]*0.5;
      }
   }

   build_height_array();

   add_lighting_detail();

   compute_ground_lighting(4,24);
}

void place_trees(void)
{
   int i,j;
   for (i=6; i+7 < MAP_X; i += 6) {
      float density = stb_linear_remap(i,0,MAP_X, 1.5, -1.0 / 20);
      density = smoothstep(density) * 0.7;
      for (j=6; j+7 < MAP_Y; j += 6) {
         if (i >= MAP_X*3/4 && j >= MAP_Y/4 && j <= MAP_Y*3/4) continue;
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

#define OUTER_WALL_HEIGHT  5
#define OUTER_WALL_COLOR   0x80404040

void draw_hwall(int x0, int x1, int y, int z0, int z1, uint32 color)
{
   int x;
   for (x=x0; x <= x1; ++x)
      draw_column(x,y,z0,z1,color);
}

void draw_vwall(int x, int y0, int y1, int z0, int z1, uint32 color)
{
   int y;
   for (y=y0; y <= y1; ++y)
      draw_column(x,y,z0,z1,color);
}

void draw_rect_wall(int x0, int y0, int x1, int y1, int h, uint32 color)
{
   int x,y;
   draw_hwall(x0,x1, y0, 0,h,color);
   draw_hwall(x0,x1, y1, 0,h,color);
   draw_vwall(x0, y0,y1, 0,h,color);
   draw_vwall(x1, y0,y1, 0,h,color);
}

void draw_rect_wall_partial(int x0, int y0, int x1, int y1, int h, uint32 color)
{
   int x,y;
   int cycle=0;
   for (x=x0; x <= x1; ++x) {
      if (cycle % 10 < 3) {
         draw_column(x,y0  ,0,h,color);
         draw_column(x,y1  ,0,h,color);
      }
      ++cycle;
   }
   for (y=y0; y <= y1; ++y) {
      if (cycle % 10 < 3) {
         draw_column(x0  ,y,0,h,color);
         draw_column(x1  ,y,0,h,color);
      }
      ++cycle;
   }
}


void draw_box(int x0, int y0, int x1, int y1, int z0, int z1, uint32 color)
{
   int x,y,z;
   for (x=x0; x <= x1; ++x)
      for (y=y0; y <= y1; ++y)
         draw_column(x,y, z0,z1, color);
}

// a pile is a collection meant for building materials
void draw_pile(int x, int y, int z, uint32 color)
{
   draw_box(x-2,y-2,x+2,y+2, z, z+3, color);  // 7*7*2 = 100 blocks, 6*6*3 = 108 blocks, 5*5*4 = 100 blocks
//   draw_box(x-1,y-1,x+1,y+1, z+2,z+4, color); // 3*3*3 = 27 blocks
}

#define BUILDING_COLOR   0x80808080

void make_building(int x0, int y0, int x1, int y1, int type)
{
   int i,j,x,y;

   #define STORY1_HEIGHT  12
   #define STORY2_HEIGHT  28
   #define CATWALK_COLOR  0x80c0a060

   // make foundation
   draw_box(x0-1,y0-1,x1+2,y1+2, 0,4,BUILDING_COLOR);
   draw_box(x0,y0,x1+1,y1+1,0,5, BUILDING_COLOR);
   draw_box(x0,y0,x1+1,y1+1,6,32, 0);

   // make wall   
   draw_rect_wall(x0,y0,x1+1,y1+1,STORY1_HEIGHT, BUILDING_COLOR);
   draw_rect_wall(x0+1,y0+1,x1,y1,STORY1_HEIGHT, BUILDING_COLOR);

   if (type == 0) {
      // split it up 3x3, and make the central region a tower
      int x2 = (x0*2 + x1) / 3 + rnd(-6,6);
      int x3 = (x0 + x1*2) / 3 + rnd(-6,6);
      int y2 = (y0*2 + y1) / 3 + rnd(-6,6);
      int y3 = (y0 + y1*2) / 3 + rnd(-6,6);

      int xpos[4] = { x0,x2,x3,x1 }, ypos[4] = { y0,y2,y3,y1 };
      int x_catwalk[3], y_catwalk[3];

      // build the internal walls
      draw_hwall(x0,x1, y2,   5, STORY1_HEIGHT, BUILDING_COLOR);
      draw_hwall(x0,x1, y2+1, 5, STORY1_HEIGHT, BUILDING_COLOR);

      draw_hwall(x0,x1, y3  , 5, STORY1_HEIGHT, BUILDING_COLOR);
      draw_hwall(x0,x1, y3+1, 5, STORY1_HEIGHT, BUILDING_COLOR);

      draw_vwall(x2,  y0, y1, 5, STORY1_HEIGHT, BUILDING_COLOR);
      draw_vwall(x2+1,y0, y1, 5, STORY1_HEIGHT, BUILDING_COLOR);

      draw_vwall(x3,  y0, y1, 5, STORY1_HEIGHT, BUILDING_COLOR);
      draw_vwall(x3+1,y0, y1, 5, STORY1_HEIGHT, BUILDING_COLOR);

      // build a doorway in every direction
      for (i=0; i < 4; ++i) {
         for (j=0; j < 4; ++j) {
            if (i < 3 && ((stb_rand() % 100) < (j==0 || j==3 ? 100u : 70u))) {
               // draw gap in a horizontal wall
               int x = rnd(xpos[i]+2, xpos[i+1]-2);
               int y = ypos[j];
               draw_box(x,y,x+1,y+1, 6,10, 0);
            }
            if (j < 3 && ((stb_rand() % 100) < (i==0 || i==3 ? 100u : 80u))) {
               // draw gap in a vertical wall
               int x = xpos[i];
               int y = rnd(ypos[j]+2, ypos[j+1]-2);
               draw_box(x,y,x+1,y+1, 6,10, 0);
            }
         }
      }

      for (i=0; i < 3; ++i) {
         x_catwalk[i] = (xpos[i] + xpos[i+1])/2 + rnd(-3,3);
         y_catwalk[i] = (ypos[i] + ypos[i+1])/2 + rnd(-3,3);
      }

      // decide whether to build a roof or not
      for (i=0; i < 3; ++i) {
         for (j=0; j < 3; ++j) {
            if (i==1 && j == 1)
               ;  // the center area is special
            else if ((stb_rand() % 100 < 40)) {
               // build a roof
               draw_box(xpos[i],ypos[j], xpos[i+1],ypos[j+1], STORY1_HEIGHT-1, STORY1_HEIGHT, BUILDING_COLOR);
            } else {
               // build catwalks
               int xm = rnd(-1,0);
               int ym = rnd(-1,0);
               int xw = rnd(0,1);
               int yw = rnd(0,1);
               draw_box(x_catwalk[i]+xm,ypos[j]+2, x_catwalk[i]+xw,ypos[j+1]-1, STORY1_HEIGHT, STORY1_HEIGHT, CATWALK_COLOR);
               draw_box(xpos[i]+2, y_catwalk[j]+ym, xpos[i+1]-1, y_catwalk[j]+yw, STORY1_HEIGHT, STORY1_HEIGHT, CATWALK_COLOR);
            }
         }
      }

      // build very thick walls around the center thing
      draw_box(x2-2,y2-2,x3+3,y3+3, STORY1_HEIGHT, STORY1_HEIGHT+1, BUILDING_COLOR);
      draw_box(x2-1,y2-1,x3+2,y3+2, STORY1_HEIGHT+2, STORY1_HEIGHT+4, BUILDING_COLOR);
      draw_box(x2  ,y2  ,x3+1,y3+1, STORY1_HEIGHT+5, STORY1_HEIGHT+7, BUILDING_COLOR);
      draw_box(x2+1,y2+1,x3+0,y3+0, STORY1_HEIGHT+8, STORY2_HEIGHT, BUILDING_COLOR);
      // dig out the interior of the tower
      draw_box(x2+2,y2+2,x3-1,y3-1, STORY1_HEIGHT-2, STORY2_HEIGHT, 0);
      // draw the tower floor
      draw_box(x2+1,y2+1, x3,y3, STORY1_HEIGHT+9, STORY1_HEIGHT+11, BUILDING_COLOR);

      // dig a hole in the floor or build stairs
      x = (x0+x1)/2;
      y = (y0+y1)/2;

      x = x2 + rnd(2,5);
      //draw_box(x-1,y-1,x+1,y+1, STORY1_HEIGHT+9, STORY1_HEIGHT+11, 0);

      draw_hstair(x-1,y,5,STORY1_HEIGHT+11, 1,0, 0x8060c060);

   } else {
      // create building materials
      int i,j;
      for (i=0; i < 4; ++i) {
         for (j=0; j < 4; ++j) {
            draw_pile(stb_linear_remap(i,-0.5,3.5, x0,x1)+rnd(-1,1), stb_linear_remap(j,-0.5,3.5,y0,y1)+rnd(-1,1), 6, 0x8060c020);
         }
      }
      // draw partial roof
      for (i=0; i < 4; ++i) {
          int x = stb_linear_remap(i,-0.5,3.5, x0,x1) + rnd(-1,1);
          int y = stb_linear_remap(i,-0.5,3.5, y0,y1) + rnd(-1,1);
          draw_box(x, y0+1, x + rnd(0,1), y1+1, STORY1_HEIGHT, STORY1_HEIGHT, CATWALK_COLOR);
          draw_box(x0+1, y, x1, y + rnd(0,1), STORY1_HEIGHT, STORY1_HEIGHT, CATWALK_COLOR);
      }
   }
}


void make_fortress(void)
{
   int x,y;

   int x0 = MAP_X*3/4 + MOAT_SIZE + 8;
   int x1 = MAP_X - MOAT_SIZE - 8;
   int y0 = MAP_Y/4 + MOAT_SIZE + 8;
   int y1 = MAP_Y*3/4 - MOAT_SIZE - 8;

   int y_split;

   // make outer wall
   draw_rect_wall(x0,y0,x1,y1,OUTER_WALL_HEIGHT, OUTER_WALL_COLOR);

   // make occasional higher walls
   draw_rect_wall_partial(x0,y0,x1,y1, OUTER_WALL_HEIGHT+4, OUTER_WALL_COLOR);

   // double-thick
   // draw_rect_wall(x0+1,x1-1,y0+1,y0-1,OUTER_WALL_HEIGHT, OUTER_WALL_COLOR);

   // make back doors
   draw_box(x1-1, MAP_Y/2-MAP_Y/8, x1,MAP_Y/2-MAP_Y/8, 1,6,0);
   draw_box(x1-1, MAP_Y/2+MAP_Y/8, x1,MAP_Y/2+MAP_Y/8, 1,6,0);

   // make front door
   draw_box(x0-2, MAP_Y/2-4, x0+2,MAP_Y/2+4, 1,11, 0x80606060);
   draw_box(x0-4, MAP_Y/2-1, x0+4,MAP_Y/2+1, 3,6, 0);

   // make the three buildings

   y_split = y0 + (y1-y0)/3 - 12;
   make_building((x0+x1)/2-32, y0+16, x1-48, y_split, 0);
   y_split = y0 + (y1-y0)/3*2 + 12;
   make_building((x0+x1)/2-32, y_split, x1-48, y1-16, 0);

   make_building((x0+x1)/2-16, y0 + (y1-y0)/3 + 12, x1 - 12, y0 + (y1-y0)/3*2 - 12, 1);
}


void build_map(char *opts[256])
{
   int tile;
   int n,i,j,k;

   build_ground();

   place_trees();

   make_fortress();

   build_debug();
}

