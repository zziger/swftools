/* swfoutput.cc
   Implements generation of swf files using the rfxswf lib. The routines
   in this file are called from pdf2swf.

   This file is part of swftools.

   Swftools is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   Swftools is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with swftools; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "swfoutput.h"
#include "spline.h"
extern "C" {
#include "../lib/log.h"
#include "../lib/rfxswf.h"
}

int ignoredraworder=0;
int drawonlyshapes=0;
int jpegquality=85;

typedef unsigned char u8;
typedef unsigned short int u16;
typedef unsigned long int u32;

static int drawmode;
static int fi;
static int flag_protected;
static SWF swf;
static TAG *tag;
static int currentswfid = 0;

static SHAPE* shape;
static int shapeid = -1;
static int textid = -1;

static int fillstyleid;
static int linestyleid;
static int swflastx=0;
static int swflasty=0;
static int lastwasfill = 0;
static char* filename = 0;
static int sizex;
static int sizey;
static char fill = 0;
static char storefont = 0;
static int depth = 1;
static int startdepth = 1;
TAG* cliptags[128];
int clipshapes[128];
u32 clipdepths[128];
int clippos = 0;

int CHARMIDX = 0;
int CHARMIDY = 0;

void startshape(struct swfoutput* obj);
void starttext(struct swfoutput* obj);
void endshape();
void endtext();

// matrix multiplication. changes p0
void transform (plotxy*p0,struct swfmatrix*m)
{
    double x,y;
    x = m->m11*p0->x+m->m12*p0->y;
    y = m->m21*p0->x+m->m22*p0->y;
    p0->x = x + m->m13;
    p0->y = y + m->m23;
}

// write a move-to command into the swf
void moveto(TAG*tag, plotxy p0)
{
    int rx = (int)(p0.x*20);
    int ry = (int)(p0.y*20);
    if(rx!=swflastx || ry!=swflasty) {
      ShapeSetMove (tag, shape, rx,ry);
    }
    swflastx=rx;
    swflasty=ry;
}

// write a line-to command into the swf
void lineto(TAG*tag, plotxy p0)
{
    int rx = ((int)(p0.x*20)-swflastx);
    int ry = ((int)(p0.y*20)-swflasty);
    /* we can't skip this for rx=0,ry=0, those
       are plots */
    ShapeSetLine (tag, shape, rx,ry);
    swflastx+=rx;
    swflasty+=ry;
}

// write a spline-to command into the swf
void splineto(TAG*tag, plotxy control,plotxy end)
{
    int cx = ((int)(control.x*20)-swflastx);
    int cy = ((int)(control.y*20)-swflasty);
    swflastx += cx;
    swflasty += cy;
    int ex = ((int)(end.x*20)-swflastx);
    int ey = ((int)(end.y*20)-swflasty);
    swflastx += ex;
    swflasty += ey;
    ShapeSetCurve(tag, shape, cx,cy,ex,ey);
}

/* write a line, given two points and the transformation
   matrix. */
void line(TAG*tag, plotxy p0, plotxy p1, struct swfmatrix*m)
{
    transform(&p0,m);
    transform(&p1,m);
    moveto(tag, p0);
    lineto(tag, p1);
}

/* write a cubic (!) spline. This involves calling the approximate()
   function out of spline.cc to convert it to a quadratic spline.  */
void spline(TAG*tag,plotxy p0,plotxy p1,plotxy p2,plotxy p3,struct swfmatrix*m)
{
    double d;
    struct qspline q[16];
    int num;
    int t;
    transform(&p0,m);
    transform(&p1,m);
    transform(&p2,m);
    transform(&p3,m);

    num = approximate(p0,p1,p2,p3,q);
    for(t=0;t<num;t++) {
	moveto(tag,q[t].start);
	splineto(tag,q[t].control, q[t].end);
    }
}

/* draw a T1 outline. These are generated by pdf2swf and by t1lib.
   (representing characters) */
void drawpath(TAG*tag, T1_OUTLINE*outline, struct swfmatrix*m)
{
    if(tag->id != ST_DEFINEFONT &&
	tag->id != ST_DEFINESHAPE &&
	tag->id != ST_DEFINESHAPE2 &&
	tag->id != ST_DEFINESHAPE3)
    {
	logf("<error> internal error: drawpath needs a shape tag, not %d\n",tag->id);
	exit(1);
    }
    int log = 0;

    double x=0,y=0;
    double lastx=0,lasty=0;
    double firstx=0,firsty=0;
    int init=1;
    if(log) printf("shape-start %d\n", fill);

    while (outline)
    {
	x += (outline->dest.x/(float)0xffff);
	y += (outline->dest.y/(float)0xffff);
	if(outline->type == T1_PATHTYPE_MOVE)
	{
	    if(((int)(lastx*20) != (int)(firstx*20) ||
		(int)(lasty*20) != (int)(firsty*20)) &&
		     fill)
	    {
		plotxy p0;
		plotxy p1;
		p0.x=lastx;
		p0.y=lasty;
		p1.x=firstx;
		p1.y=firsty;
		if(log) printf("fix: %f,%f -> %f,%f\n",p0.x,p0.y,p1.x,p1.y);
		line(tag, p0, p1, m);
	    }
	    firstx=x;
	    firsty=y;
	}
	else if(outline->type == T1_PATHTYPE_LINE) 
	{
	    plotxy p0;
	    plotxy p1;
	    p0.x=lastx;
	    p0.y=lasty;
	    p1.x=x;
	    p1.y=y;
	    if(log) printf("line: %f,%f -> %f,%f\n",p0.x,p0.y,p1.x,p1.y);
	    line(tag, p0,p1,m);
	}
	else if(outline->type == T1_PATHTYPE_BEZIER)
	{
	    plotxy p0;
	    plotxy p1;
	    plotxy p2;
	    plotxy p3;
	    T1_BEZIERSEGMENT*o2 = (T1_BEZIERSEGMENT*)outline;
	    p0.x=x; 
	    p0.y=y;
	    p1.x=o2->C.x/(float)0xffff+lastx;
	    p1.y=o2->C.y/(float)0xffff+lasty;
	    p2.x=o2->B.x/(float)0xffff+lastx;
	    p2.y=o2->B.y/(float)0xffff+lasty;
	    p3.x=lastx;
	    p3.y=lasty;
	    if(log) printf("spline: %f,%f -> %f,%f\n",p3.x,p3.y,p0.x,p0.y);
	    spline(tag,p0,p1,p2,p3,m);
	} 
	else {
	 logf("<error> drawpath: unknown outline type:%d\n", outline->type);
	}
	lastx=x;
	lasty=y;
	outline = outline->link;
    }
    if(((int)(lastx*20) != (int)(firstx*20) ||
	(int)(lasty*20) != (int)(firsty*20)) &&
	     fill)
    {
	plotxy p0;
	plotxy p1;
	p0.x=lastx;
	p0.y=lasty;
	p1.x=firstx;
	p1.y=firsty;
	if(log) printf("fix: %f,%f -> %f,%f\n",p0.x,p0.y,p1.x,p1.y);
	line(tag, p0, p1, m);
    }
    if(log) printf("shape-end\n");
}

int colorcompare(RGBA*a,RGBA*b)
{

    if(a->r!=b->r ||
       a->g!=b->g ||
       a->b!=b->b ||
       a->a!=b->a) {
	return 0;
    }
    return 1;
}

static const int CHARDATAMAX = 1024;
struct chardata {
    int charid;
    int fontid;
    int x;
    int y;
    int size;
    RGBA color;
} chardata[CHARDATAMAX];
int chardatapos = 0;

int once=0;
void putcharacters(TAG*tag)
{
    int t;
    SWFFONT font;
    RGBA color;
    color.r = chardata[0].color.r^255;
    color.g = 0;
    color.b = 0;
    color.a = 0;
    int lastfontid;
    int lastx;
    int lasty;
    int lastsize;
    int charids[128];
    int charadvance[128];
    int charstorepos;
    int pass;
    int glyphbits=1; //TODO: can this be zero?
    int advancebits=1;

    if(tag->id != ST_DEFINETEXT &&
	tag->id != ST_DEFINETEXT2) {
	logf("<error> internal error: putcharacters needs an text tag, not %d\n",tag->id);
	exit(1);
    }

    for(pass = 0; pass < 2; pass++)
    {
	charstorepos = 0;
	lastfontid = -1;
	lastx = CHARMIDX;
	lasty = CHARMIDY;
	lastsize = -1;

	if(pass==1)
	{
	    advancebits++;
	    SetU8(tag, glyphbits);
	    SetU8(tag, advancebits);
        }

	for(t=0;t<=chardatapos;t++)
	{
	    if(lastfontid != chardata[t].fontid || 
		    lastx!=chardata[t].x ||
		    lasty!=chardata[t].y ||
		    !colorcompare(&color, &chardata[t].color) ||
		    charstorepos==127 ||
		    lastsize != chardata[t].size ||
		    t == chardatapos)
	    {
		if(charstorepos && pass==0)
		{
		    int s;
		    for(s=0;s<charstorepos;s++)
		    {
			while(charids[s]>=(1<<glyphbits))
			    glyphbits++;
			while(charadvance[s]>=(1<<advancebits))
			    advancebits++;
		    }
		}
		if(charstorepos && pass==1)
		{
		    tag->bitcount = 0;
		    SetBits(tag, 0, 1); // GLYPH Record
		    SetBits(tag, charstorepos, 7); // one glyph
		    int s;
		    for(s=0;s<charstorepos;s++)
		    {
			SetBits(tag, charids[s], glyphbits);
			SetBits(tag, charadvance[s], advancebits);
		    }
		}
		charstorepos = 0;

		if(pass == 1 && t<chardatapos)
		{
		    RGBA*newcolor=0;
		    SWFFONT*newfont=0;
		    int newx = 0;
		    int newy = 0;
		    if(lastx != chardata[t].x ||
		       lasty != chardata[t].y)
		    {
			newx=chardata[t].x;
			newy=chardata[t].y;
		    }
		    if(!colorcompare(&color, &chardata[t].color)) 
		    {
			color = chardata[t].color;
			newcolor = &color;
		    }
		    font.id = chardata[t].fontid;
		    if(lastfontid != chardata[t].fontid || lastsize != chardata[t].size)
			newfont = &font;

		    tag->bitcount = 0;
		    TextSetInfoRecord(tag, newfont, chardata[t].size, newcolor, newx,newy);
		}

		lastfontid = chardata[t].fontid;
		lastx = chardata[t].x;
		lasty = chardata[t].y;
		lastsize = chardata[t].size;
	    }

	    if(t==chardatapos)
		    break;

	    int advance;
	    int nextt = t==chardatapos-1?t:t+1;
	    int rel = chardata[nextt].x-chardata[t].x;
	    if(rel>=0 && (rel<(1<<(advancebits-1)) || pass==0)) {
	       advance = rel;
	       lastx=chardata[nextt].x;
	    }
	    else {
	       advance = 0;
	       lastx=chardata[t].x;
	    }
	    charids[charstorepos] = chardata[t].charid;
	    charadvance[charstorepos] = advance;
	    charstorepos ++;
	}
    }
    chardatapos = 0;
}

void putcharacter(struct swfoutput*obj, int fontid, int charid, int x,int y, int size)
{
    if(chardatapos == CHARDATAMAX)
    {
	endtext();
	starttext(obj);
    }
    chardata[chardatapos].fontid = fontid;
    chardata[chardatapos].charid = charid;
    chardata[chardatapos].x = x;
    chardata[chardatapos].y = y;
    chardata[chardatapos].color = obj->fillrgb;
    chardata[chardatapos].size = size;
    chardatapos++;
}


/* process a character. */
void drawchar(struct swfoutput*obj, SWFFont*font, char*character, swfmatrix*m)
{
    int usefonts=1;
    if(m->m12!=0 || m->m21!=0)
	usefonts=0;
    if(m->m11 != m->m22)
	usefonts=0;

    if(usefonts && ! drawonlyshapes)
    {
	int charid = font->getSWFCharID(character);
	if(shapeid>=0)
	    endshape();
	if(textid<0)
	    starttext(obj);
	putcharacter(obj, font->swfid, charid, (int)(m->m13*20),(int)(m->m23*20),
		(int)(m->m11*20/2+0.5)); //where does the /2 come from?
    }
    else
    {
	T1_OUTLINE*outline = font->getOutline(character);
	char* charname = character;

	if(!outline) {
	 logf("Didn't find %s in current charset (%s)", character,font->getName());
	 return;
	}
	
	swfmatrix m2=*m;    
	m2.m11/=100;
	m2.m21/=100;
	m2.m12/=100;
	m2.m22/=100;

	if(textid>=0)
	    endtext();
	if(shapeid<0)
	    startshape(obj);

	if(!lastwasfill)
	 ShapeSetStyle(tag,shape,0x8000,fillstyleid,0);
	lastwasfill = 1;

	int lf = fill;
	fill = 1;
	drawpath(tag, outline, &m2);
	fill = lf;
    }
}

/* draw a curved polygon. */
void swfoutput_drawpath(swfoutput*output, T1_OUTLINE*outline, struct swfmatrix*m)
{
    if(textid>=0)
	endtext();
    if(shapeid<0)
	startshape(output);

    if(lastwasfill && !fill)
    {
     ShapeSetStyle(tag,shape,linestyleid,0x8000,0);
     lastwasfill = 0;
    }
    if(!lastwasfill && fill)
    {
     ShapeSetStyle(tag,shape,0x8000,fillstyleid,0);
     lastwasfill = 1;
    }

    drawpath(tag, outline,m); 
}

/* SWFFont: copy all t1 font outlines to a local 
   array. */
SWFFont::SWFFont(char*name, int id, char*filename)
{
    if(!T1_GetFontName(id))
	T1_LoadFont(id);

    this->name = strdup(T1_GetFontFileName(id));
    this->fontid = strdup(name);
    this->t1id = id;

    char**a= T1_GetAllCharNames(id);
    int t=0, outlinepos=0;
    char*map[256];
    while(a[t])
	t++;
 
    this->charnum = t;
    if(!t) 
	return;
    logf("<verbose> Font %s(%d): Storing %d outlines.\n", name, id, t);
    
    outline = (T1_OUTLINE**)malloc(t*sizeof(T1_OUTLINE*));
    charname = (char**)malloc(t*sizeof(char*));
    used = (char*)malloc(t*sizeof(char));
    char2swfcharid = (U16*)malloc(t*2);
    swfcharid2char = (U16*)malloc(t*2);
    swfcharpos = 0;

    memset(used,0,t*sizeof(char));

    this->swfid = ++currentswfid;

    
    t=0;
    while(*a)
    {
	map[t] = *a;
	a++;
	t++;
	if(t==256 || !*a) {
	    int s;
	    for(s=t;s<256;s++)
		map[s] = ".notdef";

	    int ret = T1_ReencodeFont(id, map);
	    if(ret) {
	     T1_DeleteFont(id);
	     T1_LoadFont(id);
	     int ret = T1_ReencodeFont(id, map);
	     if(ret)
	       fprintf(stderr,"Can't reencode font: (%s) ret:%d\n",filename, ret);
	    }

	    // parsecharacters
	    for(s=0;s<t;s++)
	    {
		this->outline[outlinepos] = T1_CopyOutline(T1_GetCharOutline(id, s, 100.0, 0));
		this->charname[outlinepos] = strdup(T1_GetCharName(id, s));
		outlinepos++;
	    }
	    t=0;
	}
    }
}

/* free all tables, write out definefont tags */
SWFFont::~SWFFont()
{
    int t,usednum=0;
    int*ptr = (int*)malloc(swfcharpos*sizeof(int));

    for(t=0;t<charnum;t++)
	if(used[t]) usednum++;

    if(usednum && !drawonlyshapes)
    {
	logf("<verbose> Font %s has %d used characters",fontid, usednum);
	TAG*ftag = InsertTag(swf.FirstTag,ST_DEFINEFONT);
	SetU16(ftag, this->swfid);
	int initpos = GetDataSize(ftag);
	swfmatrix m;
	m.m11 = m.m22 = 1;
	m.m21 = m.m12 = 0;
	m.m13 = CHARMIDX;
	m.m23 = CHARMIDY;
	for(t=0;t<swfcharpos;t++) 
	{
	    ptr[t] = GetDataSize(ftag);
	    SetU16(ftag, 0x1234);
	}
	for(t=0;t<swfcharpos;t++)
	{
	    *(U16*)&ftag->data[ptr[t]] = GetDataSize(ftag)-initpos;
	    swflastx=0;
	    swflasty=0;
	    SetU8(ftag,0x10); //0 fill bits, 0 linestyle bits
	    SHAPE s;
	    s.bits.fill = 1;
	    s.bits.line = 0;
	    ShapeSetStyle(ftag,&s,0,1,0);
	    int lastfill = fill;
	    fill = 1;
	    storefont = 1;
	    drawpath(ftag, outline[swfcharid2char[t]],&m);
	    storefont = 0;
	    fill = lastfill;
	    ShapeSetEnd(ftag);
	}
    }

    free(ptr);
    free(outline);
    for(t=0;t<charnum;t++)
	free(charname[t]);
    free(charname);
    free(used);
    free(swfcharid2char);
    free(char2swfcharid);
}

T1_OUTLINE*SWFFont::getOutline(char*name)
{
    int t;
    for(t=0;t<this->charnum;t++) {
	if(!strcmp(this->charname[t],name)) {

	    if(!used[t])
	    {
		swfcharid2char[swfcharpos] = t;
		char2swfcharid[t] = swfcharpos;
		swfcharpos++;
		used[t] = 1;
	    }
	    return outline[t];
	}
    }
    return 0;
}

int SWFFont::getSWFCharID(char*name)
{
    int t;
    for(t=0;t<this->charnum;t++) {
	if(!strcmp(this->charname[t],name)) {
	   
	    if(!used[t])
	    {
		swfcharid2char[swfcharpos] = t;
		char2swfcharid[t] = swfcharpos++;
		used[t] = 1;
	    }
	    return char2swfcharid[t];
	}
    }
    return 0;
}

char*SWFFont::getName()
{
    return this->name;
}

struct fontlist_t 
{
    SWFFont * font;
    fontlist_t*next;
} *fontlist = 0;

/* set's the t1 font index of the font to use for swfoutput_drawchar(). */
void swfoutput_setfont(struct swfoutput*obj, char*fontid, int t1id, char*filename)
{
    fontlist_t*last=0,*iterator;
    if(obj->font && !strcmp(obj->font->fontid,fontid))
	return;

    iterator = fontlist;
    while(iterator) {
	if(!strcmp(iterator->font->fontid,fontid))
	    break;
	last = iterator;
	iterator = iterator->next;
    }
    if(iterator) 
    {
	obj->font = iterator->font;
	return ;
    }

    if(t1id<0) {
	logf("<error> internal error: t1id:%d, fontid:%s\n", t1id,fontid);
    }
    
    SWFFont*font = new SWFFont(fontid, t1id, filename);
    iterator = new fontlist_t;
    iterator->font = font;
    iterator->next = 0;

    if(last) 
	last->next = iterator;
    else 
	fontlist = iterator;
    obj->font = font;
}

int swfoutput_queryfont(struct swfoutput*obj, char*fontid)
{
    fontlist_t *iterator = fontlist;
    while(iterator) {
	if(!strcmp(iterator->font->fontid,fontid))
	    return 1;
	iterator = iterator->next;
    }
    return 0;
}

/* set's the matrix which is to be applied to characters drawn by
   swfoutput_drawchar() */
void swfoutput_setfontmatrix(struct swfoutput*obj,double m11,double m12,
                                                  double m21,double m22)
{
    if(obj->fontm11 == m11 &&
       obj->fontm12 == m12 &&
       obj->fontm21 == m21 &&
       obj->fontm22 == m22)
	return;
//    if(textid>=0)
//	endtext();
    obj->fontm11 = m11;
    obj->fontm12 = m12;
    obj->fontm21 = m21;
    obj->fontm22 = m22;
}

/* draws a character at x,y. */
void swfoutput_drawchar(struct swfoutput* obj,double x,double y,char*character) 
{
    swfmatrix m;
    m.m11 = obj->fontm11;
    m.m12 = obj->fontm12;
    m.m21 = obj->fontm21;
    m.m22 = obj->fontm22;
    m.m13 = x;
    m.m23 = y;
    drawchar(obj, obj->font, character, &m);
}

/* initialize the swf writer */
void swfoutput_init(struct swfoutput* obj, char*_filename, int _sizex, int _sizey) 
{
  GLYPH *glyph;
  RGBA rgb;
  SRECT r;
  memset(obj, 0, sizeof(struct swfoutput));
  filename = _filename;
  sizex = _sizex;
  sizey = _sizey;

  logf("<verbose> initializing swf output for size %d*%d\n", sizex,sizey);

  obj->font = 0;
  
  memset(&swf,0x00,sizeof(SWF));

  swf.FileVersion    = 4;
//  swf.FrameRate      = 0x1900;
  swf.FrameRate      = 0x0040; // 1 frame per 4 seconds
  swf.MovieSize.xmax = 20*sizex;
  swf.MovieSize.ymax = 20*sizey;
  
  swf.FirstTag = InsertTag(NULL,ST_SETBACKGROUNDCOLOR);
  tag = swf.FirstTag;
  rgb.r = 0xff;
  rgb.g = 0xff;
  rgb.b = 0xff;
  SetRGB(tag,&rgb);
  if(flag_protected)
    tag = InsertTag(tag, ST_PROTECT);
  depth = 1;
  startdepth = depth;
}

void swfoutput_setprotected() //write PROTECT tag
{
  flag_protected = 1;
}

void startshape(struct swfoutput*obj)
{
  RGBA rgb;
  SRECT r;

  if(textid>=0)
      endtext();

  tag = InsertTag(tag,ST_DEFINESHAPE);

  NewShape(&shape);
  linestyleid = ShapeAddLineStyle(shape,obj->linewidth,&obj->strokergb);
  rgb.r = obj->fillrgb.r;
  rgb.g = obj->fillrgb.g;
  rgb.b = obj->fillrgb.b;
  fillstyleid = ShapeAddSolidFillStyle(shape,&obj->fillrgb);

  shapeid = ++currentswfid;
  SetU16(tag,shapeid);  // ID

  r.xmin = 0;
  r.ymin = 0;
  r.xmax = 20*sizex;
  r.ymax = 20*sizey;
  
  SetRect(tag,&r);

  SetShapeStyles(tag,shape);
  ShapeCountBits(shape,NULL,NULL);
  SetShapeBits(tag,shape);

  ShapeSetAll(tag,shape,/*x*/0,/*y*/0,linestyleid,0,0);
  swflastx=swflasty=0;
  lastwasfill = 0;
}

void starttext(struct swfoutput*obj)
{
  SRECT r;
  MATRIX m;
  if(shapeid>=0)
      endshape();
  tag = InsertTag(tag,ST_DEFINETEXT);
  textid = ++currentswfid;
  SetU16(tag, textid);

  r.xmin = 0;
  r.ymin = 0;
  r.xmax = 20*sizex;
  r.ymax = 20*sizey;
  
  SetRect(tag,&r);

  m.sx = 65536;
  m.sy = 65536;
  m.r0 = 0;
  m.r1 = 0;
  m.tx = 0;
  m.ty = 0;
 
  SetMatrix(tag,&m);
  swflastx=swflasty=0;
}

void endshape()
{
    if(shapeid<0) 
	return;
    ShapeSetEnd(tag);
    tag = InsertTag(tag,ST_PLACEOBJECT2);
    ObjectPlace(tag,shapeid,/*depth*/depth++,NULL,NULL,NULL);
    shapeid = -1;
}

void endtext()
{
    if(textid<0)
	return;
    putcharacters(tag);
    SetU8(tag,0);
    tag = InsertTag(tag,ST_PLACEOBJECT2);
    ObjectPlace(tag,textid,/*depth*/depth++,NULL,NULL,NULL);
    textid = -1;
}

void endpage(struct swfoutput*obj)
{
    if(shapeid>=0)
      endshape();
    if(textid>=0)
      endtext();
    while(clippos)
	swfoutput_endclip(obj);
    tag = InsertTag(tag,ST_SHOWFRAME);
}

void swfoutput_newpage(struct swfoutput*obj)
{
    endpage(obj);

    for(depth--;depth>=startdepth;depth--) {
	tag = InsertTag(tag,ST_REMOVEOBJECT2);
	SetU16(tag,depth);
    }

    depth = 1;
    startdepth = depth;
}

/* "destroy" like in (oo-terminology) "destructor". Perform cleaning
   up, complete the swf, and write it out. */
void swfoutput_destroy(struct swfoutput* obj) 
{
    endpage(obj);
    fontlist_t *tmp,*iterator = fontlist;
    while(iterator) {
	delete iterator->font;
	iterator->font = 0;
	tmp = iterator;
	iterator = iterator->next;
	delete tmp;
    }

    T1_CloseLib();
    if(!filename) 
	return;
    if(filename)
     fi = open(filename, O_CREAT|O_TRUNC|O_WRONLY, 0777);
    else
     fi = 1; // stdout
    
    if(fi<=0) {
     logf("<fatal> Could not create \"%s\". ", filename);
     exit(1);
    }
 
    tag = InsertTag(tag,ST_END);

    if FAILED(WriteSWF(fi,&swf)) 
     logf("<error> WriteSWF() failed.\n");
    if(filename)
     close(fi);
    logf("<notice> SWF written\n");
}

void swfoutput_setdrawmode(swfoutput* obj, int mode)
{
    drawmode = mode;
    if(mode == DRAWMODE_FILL)
     fill = 1;
    else if(mode == DRAWMODE_EOFILL)
     fill = 1;
    else if(mode == DRAWMODE_STROKE)
     fill = 0;
    else if(mode == DRAWMODE_CLIP)
     fill = 1;
    else if(mode == DRAWMODE_EOCLIP)
     fill = 1;
}

void swfoutput_setfillcolor(swfoutput* obj, u8 r, u8 g, u8 b, u8 a)
{
    if(obj->fillrgb.r == r &&
       obj->fillrgb.g == g &&
       obj->fillrgb.b == b &&
       obj->fillrgb.a == a) return;

    if(shapeid>=0)
     endshape();
    obj->fillrgb.r = r;
    obj->fillrgb.g = g;
    obj->fillrgb.b = b;
    obj->fillrgb.a = a;
}

void swfoutput_setstrokecolor(swfoutput* obj, u8 r, u8 g, u8 b, u8 a)
{
    if(obj->strokergb.r == r &&
       obj->strokergb.g == g &&
       obj->strokergb.b == b &&
       obj->strokergb.a == a) return;

    if(shapeid>=0)
     endshape();
    obj->strokergb.r = r;
    obj->strokergb.g = g;
    obj->strokergb.b = b;
    obj->strokergb.a = a;
}

void swfoutput_setlinewidth(struct swfoutput*obj, double linewidth)
{
    if(obj->linewidth == (u16)(linewidth*20))
	return;

    if(shapeid>=0)
     endshape();
    obj->linewidth = (u16)(linewidth*20);
}


void swfoutput_startclip(swfoutput*obj, T1_OUTLINE*outline, struct swfmatrix*m)
{
    if(textid>=0)
     endtext();
    if(shapeid>=0)
     endshape();

    if(clippos >= 127)
    {
	logf("<warning> Too many clip levels.");
	clippos --;
    } 

    startshape(obj);
    swfoutput_setdrawmode(obj, DRAWMODE_CLIP);
    swfoutput_drawpath(obj, outline, m);
    ShapeSetEnd(tag);

    tag = InsertTag(tag,ST_PLACEOBJECT2);
    cliptags[clippos] = tag;
    clipshapes[clippos] = shapeid;
    clipdepths[clippos] = depth++;
    clippos++;
    shapeid = -1;
}

void swfoutput_endclip(swfoutput*obj)
{
    if(textid>=0)
     endtext();
    if(shapeid>=0)
     endshape();

    if(!clippos) {
	logf("<error> Invalid end of clipping region");
	return;
    }
    clippos--;
    PlaceObject(cliptags[clippos],clipshapes[clippos],clipdepths[clippos],NULL,NULL,NULL,depth++);
}

void swfoutput_drawimagefile(struct swfoutput*, char*filename, int sizex,int sizey, 
	double x1,double y1,
	double x2,double y2,
	double x3,double y3,
	double x4,double y4)
{
    if(shapeid>=0)
     endshape();
    if(textid>=0)
     endtext();

    RGBA rgb;
    SRECT r;
    int lsid=0;
    int fsid;
    int bitid;
    struct plotxy p1,p2,p3,p4;
    int myshapeid;
    double xmax=x1,ymax=y1,xmin=x1,ymin=y1;
    if(x2>xmax) xmax=x2;
    if(y2>ymax) ymax=y2;
    if(x2<xmin) xmin=x2;
    if(y2<ymin) ymin=y2;
    if(x3>xmax) xmax=x3;
    if(y3>ymax) ymax=y3;
    if(x3<xmin) xmin=x3;
    if(y3<ymin) ymin=y3;
    if(x4>xmax) xmax=x4;
    if(y4>ymax) ymax=y4;
    if(x4<xmin) xmin=x4;
    if(y4<ymin) ymin=y4;
    p1.x=x1;
    p1.y=y1;
    p2.x=x2;
    p2.y=y2;
    p3.x=x3;
    p3.y=y3;
    p4.x=x4;
    p4.y=y4;
    
    MATRIX m;
    m.sx = (int)(65536*20*(x4-x1))/sizex;
    m.r1 = -(int)(65536*20*(y4-y1))/sizex;
    m.r0 = (int)(65536*20*(x1-x2))/sizey;
    m.sy = -(int)(65536*20*(y1-y2))/sizey;

    m.tx = (int)(x1*20);
    m.ty = (int)(y1*20);

    bitid = ++currentswfid;
  
    /* bitmap */
    tag = InsertTag(tag,ST_DEFINEBITSJPEG2);
    SetU16(tag, bitid);
    SetJPEGBits(tag, filename, jpegquality);

    /* shape */
    myshapeid = ++currentswfid;
    tag = InsertTag(tag,ST_DEFINESHAPE);
    NewShape(&shape);
    //lsid = ShapeAddLineStyle(shape,obj->linewidth,&obj->strokergb);
    //fsid = ShapeAddSolidFillStyle(shape,&obj->fillrgb);
    fsid = ShapeAddBitmapFillStyle(shape,&m,bitid,0);
    SetU16(tag, myshapeid);
    r.xmin = (int)(xmin*20);
    r.ymin = (int)(ymin*20);
    r.xmax = (int)(xmax*20);
    r.ymax = (int)(ymax*20);
    SetRect(tag,&r);
    SetShapeStyles(tag,shape);
    ShapeCountBits(shape,NULL,NULL);
    SetShapeBits(tag,shape);
    ShapeSetAll(tag,shape,/*x*/0,/*y*/0,lsid,fsid,0);
    swflastx = swflasty = 0;
    moveto(tag, p1);
    lineto(tag, p2);
    lineto(tag, p3);
    lineto(tag, p4);
    lineto(tag, p1);
    /*
    ShapeMoveTo  (tag, shape, (int)(x1*20),(int)(y1*20));
    ShapeSetLine (tag, shape, (int)(x1*20);
    ShapeSetLine (tag, shape, x*20,0);
    ShapeSetLine (tag, shape, 0,-y*20);
    ShapeSetLine (tag, shape, -x*20,0);*/
    ShapeSetEnd(tag);

    /* instance */
    tag = InsertTag(tag,ST_PLACEOBJECT2);
    ObjectPlace(tag,myshapeid,/*depth*/depth++,NULL,NULL,NULL);
}

