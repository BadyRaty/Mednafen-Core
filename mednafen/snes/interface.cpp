/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "../mednafen.h"
#include "../md5.h"
#include "../general.h"
#include <../base.hpp>
#include "../mempatcher.h"
#include "../PSFLoader.h"
#include "../player.h"
#include "../FileStream.h"
#include "../resampler/resampler.h"
#include <vector>

extern MDFNGI EmulatedSNES;

static void Cleanup(void);

static SpeexResamplerState *resampler = NULL;
static int32 ResampInPos;
static int16 ResampInBuffer[2048][2];
static bool PrevFrameInterlaced;
static int PrevLine;

class SNSFLoader : public PSFLoader
{
 public:

 SNSFLoader(MDFNFILE *fp);
 virtual ~SNSFLoader();

 virtual void HandleEXE(const uint8 *data, uint32 len, bool ignore_pcsp = false);
 virtual void HandleReserved(const uint8 *data, uint32 len);

 PSFTags tags;
 std::vector<uint8> ROM_Data;
};

static bSNES_v059::Interface Interface;
static SNSFLoader *snsf_loader = NULL;

static bool InProperEmu;
static bool SoundOn;
static double SoundLastRate = 0;

static int32 CycleCounter;
static MDFN_Surface *tsurf = NULL;
static MDFN_Rect *tlw = NULL;
static MDFN_Rect *tdr = NULL;
static EmulateSpecStruct *es = NULL;

static int InputType[2];
static uint8 *InputPtr[8] = { NULL };
static uint16 PadLatch[8];
static bool MultitapEnabled[2];
static bool HasPolledThisFrame;

static int16 MouseXLatch[2];
static int16 MouseYLatch[2];
static uint8 MouseBLatch[2];

static uint8 *CustomColorMap = NULL;
//static uint32 ColorMap[32768];
static std::vector<uint32> ColorMap;

static bool LoadCPalette(const char *syspalname, uint8 **ptr, uint32 num_entries)
{
 std::string colormap_fn = MDFN_MakeFName(MDFNMKF_PALETTE, 0, syspalname).c_str();

 MDFN_printf(_("Loading custom palette from \"%s\"...\n"),  colormap_fn.c_str());
 MDFN_indent(1);

 *ptr = NULL;
 try
 {
  FileStream fp(colormap_fn.c_str(), FileStream::MODE_READ);

  if(!(*ptr = (uint8 *)MDFN_malloc(num_entries * 3, _("custom color map"))))
  {
   MDFN_indent(-1);
   return(false);
  }

  fp.read(*ptr, num_entries * 3);
 }
 catch(MDFN_Error &e)
 {
  if(*ptr)
  {
   MDFN_free(*ptr);
   *ptr = NULL;
  }

  MDFN_printf(_("Error: %s\n"), e.what());
  MDFN_indent(-1);
  return(e.GetErrno() == ENOENT);        // Return fatal error if it's an error other than the file not being found.
 }
 catch(std::exception &e)
 {
  if(*ptr)
  {
   MDFN_free(*ptr);
   *ptr = NULL;
  }

  MDFN_printf(_("Error: %s\n"), e.what());
  MDFN_indent(-1);
  return(false);
 }

 MDFN_indent(-1);

 return(true);
}


static void BuildColorMap(MDFN_PixelFormat &format)
{
 for(int x = 0; x < 32768; x++) 
 {
  int r, g, b;

  r = (x & (0x1F <<  0)) << 3;
  g = (x & (0x1F <<  5)) >> (5 - 3);
  b = (x & (0x1F << 10)) >> (5 * 2 - 3);

  //r = ((((x >> 0) & 0x1F) * 255 + 15) / 31);
  //g = ((((x >> 5) & 0x1F) * 255 + 15) / 31);
  //b = ((((x >> 10) & 0x1F) * 255 + 15) / 31);

  if(CustomColorMap)
  {
   r = CustomColorMap[x * 3 + 0];
   g = CustomColorMap[x * 3 + 1];
   b = CustomColorMap[x * 3 + 2];
  }

  ColorMap[x] = format.MakeColor(r, g, b);
 }
}


static void BlankMissingLines(int ystart, int ybound, const bool interlaced, const bool field)
{
 for(int y = ystart; y < ybound; y++)
 {
  //printf("Blanked: %d\n", y);
  uint32 *dest_line = tsurf->pixels + (field * tsurf->pitch32) + (y * tsurf->pitch32);
  dest_line[0] = tsurf->MakeColor(0, 0/*rand() & 0xFF*/, 0);
  tlw[(y << interlaced) + field].x = 0;
  tlw[(y << interlaced) + field].w = 1;
 }
}

void bSNES_v059::Interface::video_scanline(uint16_t *data, unsigned line, unsigned width, unsigned height, bool interlaced, bool field)
{
 const int ppline = PrevLine;

 //if(rand() & 1)
 // return;

 if((int)line <= PrevLine || (PrevLine == -1 && line > 32)) // Second part for PAL 224 line mode
  return;

 PrevLine = line;
 PrevFrameInterlaced = interlaced;

 if(snsf_loader)
  return;

 if(!tsurf || !tlw || !tdr)
  return;

 if(es->skip && !interlaced)
  return;

 if(!interlaced)
  field = 0;


 BlankMissingLines(ppline + 1, line, interlaced, field);
 //if(line == 0)
 // printf("ZOOM: 0x%04x, %d %d, %d\n", data[0], interlaced, field, width);

 const unsigned y = line;
 const uint16 *source_line = data;
 uint32 *dest_line = tsurf->pixels + (field * tsurf->pitch32) + ((y << interlaced) * tsurf->pitch32);

 //if(rand() & 1)
 {
  tlw[(y << interlaced) + field].x = 0;
  tlw[(y << interlaced) + field].w = width;
 }

 if(width == 512 && (source_line[0] & 0x8000))
 {
  tlw[(y << interlaced) + field].w = 256;
  for(int x = 0; x < 256; x++)
  {
   uint16 p1 = source_line[(x << 1) | 0] & 0x7FFF;
   uint16 p2 = source_line[(x << 1) | 1] & 0x7FFF;
   dest_line[x] = ColorMap[(p1 + p2 - ((p1 ^ p2) & 0x0421)) >> 1];
  }
 }
 else
 {
  for(int x = 0; x < width; x++)
   dest_line[x] = ColorMap[source_line[x] & 0x7FFF];
 }

 tdr->w = width;
 tdr->h = height << interlaced;

 es->InterlaceOn = interlaced;
 es->InterlaceField = (interlaced && field);

 MDFN_MidLineUpdate(es, (y << interlaced) + field);
}

void bSNES_v059::Interface::audio_sample(uint16_t l_sample, uint16_t r_sample)
{
 CycleCounter++;

 if(!SoundOn)
  return;

 if(ResampInPos < 2048)
 {
  //l_sample = (rand() & 0x7FFF) - 0x4000;
  //r_sample = (rand() & 0x7FFF) - 0x4000;
  ResampInBuffer[ResampInPos][0] = (int16)l_sample;
  ResampInBuffer[ResampInPos][1] = (int16)r_sample;
  ResampInPos++;
 }
 else
 {
  MDFN_DispMessage("Buffer overflow?");
 }
}

#if 0
class Input {
public:
  enum Device {
    DeviceNone,
    DeviceJoypad,
    DeviceMultitap,
    DeviceMouse,
    DeviceSuperScope,
    DeviceJustifier,
    DeviceJustifiers,
  };

  enum JoypadID {
    JoypadB      =  0, JoypadY     =  1,
    JoypadSelect =  2, JoypadStart =  3,
    JoypadUp     =  4, JoypadDown  =  5,
    JoypadLeft   =  6, JoypadRight =  7,
    JoypadA      =  8, JoypadX     =  9,
    JoypadL      = 10, JoypadR     = 11,
  };
#endif

void bSNES_v059::Interface::input_poll()
{
 if(!InProperEmu)
  return;

 HasPolledThisFrame = true;

 for(int port = 0; port < 2; port++)
 {
  switch(InputType[port])
  {
   case bSNES_v059::Input::DeviceJoypad:
	PadLatch[port] = MDFN_de16lsb(InputPtr[port]);
	break;

   case bSNES_v059::Input::DeviceMultitap:
	for(int index = 0; index < 4; index++)
        {
         if(!index)
          PadLatch[port] = MDFN_de16lsb(InputPtr[port]);
         else
	 {
	  int pi = 2 + 3 * (port ^ 1) + (index - 1);
          PadLatch[pi] = MDFN_de16lsb(InputPtr[pi]);
	 }
        }
        break;

   case bSNES_v059::Input::DeviceMouse:
	MouseXLatch[port] = (int32)MDFN_de32lsb(InputPtr[port] + 0);
	MouseYLatch[port] = (int32)MDFN_de32lsb(InputPtr[port] + 4);
	MouseBLatch[port] = *(uint8 *)(InputPtr[port] + 8);
	break;
  }
 }
}

static INLINE int16 sats32tos16(int32 val)
{
 if(val > 32767)
  val = 32767;
 if(val < -32768)
  val = -32768;

 return(val);
}

int16_t bSNES_v059::Interface::input_poll(bool port, unsigned device, unsigned index, unsigned id)
{
 if(!HasPolledThisFrame)
  printf("input_poll(...) before input_poll() for frame, %d %d %d %d\n", port, device, index, id);

 switch(device)
 {
 	case bSNES_v059::Input::DeviceJoypad:
	{
	  return((PadLatch[port] >> id) & 1);
	}
	break;

	case bSNES_v059::Input::DeviceMultitap:
	{
	 if(!index)
          return((PadLatch[port] >> id) & 1);
         else
	  return((PadLatch[2 + 3 * (port ^ 1) + (index - 1)] >> id) & 1);
	}
	break;

	case bSNES_v059::Input::DeviceMouse:
	{
	 assert(port < 2);
	 switch(id)
	 {
	  case bSNES_v059::Input::MouseX:
		return(sats32tos16(MouseXLatch[port]));
		break;

	  case bSNES_v059::Input::MouseY:
		return(sats32tos16(MouseYLatch[port]));
		break;

	  case bSNES_v059::Input::MouseLeft:
		return((int)(bool)(MouseBLatch[port] & 1));
		break;

	  case bSNES_v059::Input::MouseRight:
		return((int)(bool)(MouseBLatch[port] & 2));
		break;
	 }
	}
	break;
 }

 return(0);
}

#if 0
void bSNES_v059::Interface::init()
{


}

void bSNES_v059::Interface::term()
{


}
#endif

#if 0

namespace memory {
  extern MappedRAM cartrom, cartram, cartrtc;
  extern MappedRAM bsxflash, bsxram, bsxpram;
  extern MappedRAM stArom, stAram;
  extern MappedRAM stBrom, stBram;
  extern MappedRAM gbrom, gbram;
};

#endif

// For loading: Return false on fatal error during loading, or true on success(or file not found)
static bool SaveMemorySub(bool load, const char *extension, bSNES_v059::MappedRAM *memoryA, bSNES_v059::MappedRAM *memoryB = NULL)
{
 const std::string path = MDFN_MakeFName(MDFNMKF_SAV, 0, extension);
 std::vector<PtrLengthPair> MemToSave;

 if(load)
 {
  gzFile gp;

  errno = 0;
  gp = gzopen(path.c_str(), "rb");
  if(!gp)
  {
   ErrnoHolder ene(errno);
   if(ene.Errno() == ENOENT)
    return(true);

   MDFN_PrintError(_("Error opening save file \"%s\": %s"), path.c_str(), ene.StrError());
   return(false);
  }

  if(memoryA && memoryA->size() != 0 && memoryA->size() != -1U)
  {
   errno = 0;
   if(gzread(gp, memoryA->data(), memoryA->size()) != memoryA->size())
   {
    ErrnoHolder ene(errno);

    MDFN_PrintError(_("Error reading save file \"%s\": %s"), path.c_str(), ene.StrError());
    return(false);
   }
  }

  if(memoryB && memoryB->size() != 0 && memoryB->size() != -1U)
  {
   errno = 0;
   if(gzread(gp, memoryB->data(), memoryB->size()) != memoryB->size())
   {
    ErrnoHolder ene(errno);

    MDFN_PrintError(_("Error reading save file \"%s\": %s"), path.c_str(), ene.StrError());
    return(false);
   }
  }

  gzclose(gp);

  return(true);
 }
 else
 {
  if(memoryA && memoryA->size() != 0 && memoryA->size() != -1U)
   MemToSave.push_back(PtrLengthPair(memoryA->data(), memoryA->size()));

  if(memoryB && memoryB->size() != 0 && memoryB->size() != -1U)
   MemToSave.push_back(PtrLengthPair(memoryB->data(), memoryB->size()));

  return(MDFN_DumpToFile(path.c_str(), 6, MemToSave));
 }
}

static bool SaveLoadMemory(bool load)
{
  if(bSNES_v059::cartridge.loaded() == false)
   return(FALSE);

  bool ret = true;

  switch(bSNES_v059::cartridge.mode())
  {
    case bSNES_v059::Cartridge::ModeNormal:
    case bSNES_v059::Cartridge::ModeBsxSlotted: 
    {
      ret &= SaveMemorySub(load, "srm", &bSNES_v059::memory::cartram);
      ret &= SaveMemorySub(load, "rtc", &bSNES_v059::memory::cartrtc);
    }
    break;

    case bSNES_v059::Cartridge::ModeBsx:
    {
      ret &= SaveMemorySub(load, "srm", &bSNES_v059::memory::bsxram );
      ret &= SaveMemorySub(load, "psr", &bSNES_v059::memory::bsxpram);
    }
    break;

    case bSNES_v059::Cartridge::ModeSufamiTurbo:
    {
     ret &= SaveMemorySub(load, "srm", &bSNES_v059::memory::stAram, &bSNES_v059::memory::stBram);
    }
    break;

    case bSNES_v059::Cartridge::ModeSuperGameBoy:
    {
     ret &= SaveMemorySub(load, "sav", &bSNES_v059::memory::gbram);
     ret &= SaveMemorySub(load, "rtc", &bSNES_v059::memory::gbrtc);
    }
    break;
  }

 return(ret);
}


static bool TestMagic(const char *name, MDFNFILE *fp)
{
 if(PSFLoader::TestMagic(0x23, fp))
  return(true);

 if(strcasecmp(fp->ext, "smc") && strcasecmp(fp->ext, "swc") && strcasecmp(fp->ext, "sfc") && strcasecmp(fp->ext, "fig") &&
        strcasecmp(fp->ext, "bs") && strcasecmp(fp->ext, "st"))
 {
  return(false);
 }

 return(true);
}

static void SetupMisc(bool PAL)
{
 PrevFrameInterlaced = false;

 bSNES_v059::video.set_mode(PAL ? bSNES_v059::Video::ModePAL : bSNES_v059::Video::ModeNTSC);

 // Nominal FPS values are a bit off, FIXME(and contemplate the effect on netplay sound buffer overruns/underruns)
 MDFNGameInfo->fps = PAL ? 838977920 : 1008307711;
 MDFNGameInfo->MasterClock = MDFN_MASTERCLOCK_FIXED(32040.5);

 if(!snsf_loader)
 {
  MDFNGameInfo->nominal_width = MDFN_GetSettingB("snes.correct_aspect") ? (PAL ? 344/*354*/ : 292) : 256;
  MDFNGameInfo->nominal_height = PAL ? 239 : 224;
  MDFNGameInfo->lcm_height = MDFNGameInfo->nominal_height * 2;
 }

 ResampInPos = 0;
 SoundLastRate = 0;
}

SNSFLoader::SNSFLoader(MDFNFILE *fp)
{
 uint32 size_tmp;
 uint8 *export_ptr;

 tags = Load(0x23, 8 + 1024 * 8192, fp);

 size_tmp = ROM_Data.size();

 assert(size_tmp <= (8192 * 1024));

 export_ptr = new uint8[8192 * 1024];
 memset(export_ptr, 0x00, 8192 * 1024);
 memcpy(export_ptr, &ROM_Data[0], size_tmp);
 bSNES_v059::memory::cartrom.map(export_ptr, size_tmp);
 ROM_Data.resize(0);

 bSNES_v059::cartridge.load(bSNES_v059::Cartridge::ModeNormal);
}

SNSFLoader::~SNSFLoader()
{

}

void SNSFLoader::HandleReserved(const uint8 *data, uint32 len)
{
 uint32 o = 0;

 if(len < 9)
  return;

 while((o + 8) <= len)
 {
  uint32 header_type = MDFN_de32lsb(&data[o + 0]);
  uint32 header_size = MDFN_de32lsb(&data[o + 4]);

  printf("%08x %08x\n", header_type, header_size);

  o += 8;

  switch(header_type)
  {
   case 0xFFFFFFFF:	// EOR
	if(header_size)
	{
	 throw MDFN_Error(0, _("SNSF Reserved Section EOR has non-zero(=%u) size."), header_size);
	}

	if(o < len)
	{
	 throw MDFN_Error(0, _("SNSF Reserved Section EOR, but more data(%u bytes) available."), len - o);
	}
	break;

   default:
	throw MDFN_Error(0, _("SNSF Reserved Section Unknown/Unsupported Data Type 0x%08x"), header_type);
	break;

   case 0:	// SRAM
	{
	 uint32 srd_offset, srd_size;

	 if((len - o) < 4)
	 {
	  throw MDFN_Error(0, _("SNSF Reserved Section SRAM block, insufficient data for subheader."));
	 }
	 srd_offset = MDFN_de32lsb(&data[o]);
	 o += 4;
	 srd_size = len - o;

	 if(srd_size > 0x20000)
	 {
	  throw MDFN_Error(0, _("SNSF Reserved Section SRAM block size(=%u) is too large."), srd_size);
	 }

	 if(((uint64)srd_offset + srd_size) > 0x20000)
	 {
	  throw MDFN_Error(0, _("SNSF Reserved Section SRAM block combined offset+size(=%ull) is too large."), (unsigned long long)srd_offset + srd_size);
	 }

	 printf("SRAM(not implemented yet): %08x %08x\n", srd_offset, srd_size);
	printf("%d\n", bSNES_v059::memory::cartram.size());
	}
	break;
  }


  o += header_size;
 }

 printf("Reserved: %d\n", len);
}


void SNSFLoader::HandleEXE(const uint8 *data, uint32 size, bool ignore_pcsp)
{
 if(size < 8)
 {
  throw MDFN_Error(0, _("SNSF Missing full program section header."));
 }

 const uint32 header_offset = MDFN_de32lsb(&data[0]);
 const uint32 header_size = MDFN_de32lsb(&data[4]);
 const uint8 *rdata = &data[8];

 printf("%08x %08x\n", header_offset, header_size);

 if(header_offset > (1024 * 8192))
 {
  throw MDFN_Error(0, _("SNSF Header Field Offset(=%u) is too large."), header_offset);
 }

 if(header_size > (1024 * 8192))
 {
  throw MDFN_Error(0, _("SNSF Header Field Size(=%u) is too large."), header_size);
 }

 if(((uint64)header_offset + header_size) > (1024 * 8192))
 {
  throw MDFN_Error(0, _("SNSF Combined Header Fields Offset(=%u) + Size(=%u) is too large."), header_offset, header_size);
 }

 if((size - 8) < header_size)
 {
  throw(MDFN_Error(0, _("SNSF Insufficient data(need %u bytes, have %u bytes)"), header_size, size - 8));
 }

 if((header_offset + header_size) > ROM_Data.size())
  ROM_Data.resize(header_offset + header_size, 0x00);

 memcpy(&ROM_Data[header_offset], rdata, header_size);
}

static bool LoadSNSF(MDFNFILE *fp)
{
 bool PAL = false;

 bSNES_v059::system.init(&Interface);

 MultitapEnabled[0] = false;
 MultitapEnabled[1] = false;


 try
 {
  std::vector<std::string> SongNames;

  snsf_loader = new SNSFLoader(fp);

  SongNames.push_back(snsf_loader->tags.GetTag("title"));

  Player_Init(1, snsf_loader->tags.GetTag("game"), snsf_loader->tags.GetTag("artist"), snsf_loader->tags.GetTag("copyright"), SongNames);
 }
 catch(std::exception &e)
 {
  MDFND_PrintError(e.what());
  Cleanup();
  return 0;
 }

 bSNES_v059::system.power();
 PAL = (bSNES_v059::system.region() == bSNES_v059::System::PAL);

 SetupMisc(PAL);

 return(true);
}

static void Cleanup(void)
{
 bSNES_v059::memory::cartrom.map(NULL, 0); // So it delete[]s the pointer it took ownership of.

 if(CustomColorMap)
 {
  MDFN_free(CustomColorMap);
  CustomColorMap = NULL;
 }

 if(snsf_loader)
 {
  delete snsf_loader;
  snsf_loader = NULL;
 }

 ColorMap.resize(0);

 if(resampler)
 {
  speex_resampler_destroy(resampler);
  resampler = NULL;
 }
}

static const unsigned cheat_page_size = 1024;
template<typename T>
static void CheatMap(bool uics, uint32 addr, T& mr, uint32 offset)
{
 assert((offset + cheat_page_size) <= mr.size());
 MDFNMP_AddRAM(cheat_page_size, addr, mr.data() + offset, uics);
}

// Intended only for the MapLinear type.
template<typename T>
static void CheatMap(bool uics, uint8 bank_lo, uint8 bank_hi, uint16 addr_lo, uint16 addr_hi, T& mr, uint32 offset = 0, uint32 size = 0)
{
 assert(bank_lo <= bank_hi);
 assert(addr_lo <= addr_hi);
 if((int)mr.size() < cheat_page_size)
 {
  if((int)mr.size() > 0)
   printf("Boop: %d\n", mr.size());
  return;
 }

 uint8 page_lo = addr_lo / cheat_page_size;
 uint8 page_hi = addr_hi / cheat_page_size;
 unsigned index = 0;

 for(unsigned bank = bank_lo; bank <= bank_hi; bank++)
 {
  for(unsigned page = page_lo; page <= page_hi; page++)
  {
   if(size)
   {
    if(index >= size)
     uics = false;
    index %= size;
   }

   if((offset + index) >= mr.size())
    uics = false;

   CheatMap(uics, (bank << 16) + (page * cheat_page_size), mr, bSNES_v059::bus.mirror(offset + index, mr.size()));
   index += cheat_page_size;
  }
 }
}

static int Load(const char *name, MDFNFILE *fp)
{
 bool PAL = FALSE;

 CycleCounter = 0;

 try
 {
  if(PSFLoader::TestMagic(0x23, fp))
  {
   return LoadSNSF(fp);
  }
  // Allocate 8MiB of space regardless of actual ROM image size, to prevent malformed or corrupted ROM images
  // from crashing the bsnes cart loading code.

  const uint32 header_adjust = (((fp->size & 0x7FFF) == 512) ? 512 : 0);
  uint8 *export_ptr;
  const uint32 csize = fp->size - header_adjust;

  if(csize > (8192 * 1024))
  {
   throw MDFN_Error(0, _("SNES ROM image is too large."));
  }

  md5_context md5;

  md5.starts();
  md5.update(fp->data, fp->size);
  md5.finish(MDFNGameInfo->MD5);

  bSNES_v059::system.init(&Interface);

  //const bSNES_v059::Cartridge::Type rom_type = bSNES_v059::cartridge.detect_image_type((uint8 *)fp->data, fp->size);

  export_ptr = new uint8[8192 * 1024];
  memset(export_ptr, 0x00, 8192 * 1024);
  memcpy(export_ptr, fp->data + header_adjust, csize);

  //
  // Mirror up to an 8MB boundary so we can implement HAPPY FUNTIME YAAAAAAAY optimizations(like with SuperFX).
  //
  //uint32 st = MDFND_GetTime();
  for(uint32 a = (csize + 255) &~255; a < 8192 * 1024; a += 256)
  {
   const uint32 oa = bSNES_v059::bus.mirror(a, csize);
   //printf("%08x->%08x\n",a, oa);
   memcpy(&export_ptr[a], &export_ptr[oa], 256);
  }

  //printf("%d\n", MDFND_GetTime() - st);

  bSNES_v059::memory::cartrom.map(export_ptr, csize);

  bSNES_v059::cartridge.load(bSNES_v059::Cartridge::ModeNormal);

  bSNES_v059::system.power();

  PAL = (bSNES_v059::system.region() == bSNES_v059::System::PAL);

  SetupMisc(PAL);

  MultitapEnabled[0] = MDFN_GetSettingB("snes.input.port1.multitap");
  MultitapEnabled[1] = MDFN_GetSettingB("snes.input.port2.multitap");

  if(!SaveLoadMemory(true))
  {
   Cleanup();
   return(0);
  }

  //printf(" %d %d\n", FSettings.SndRate, resampler.max_write());

  MDFNMP_Init(cheat_page_size, (1U << 24) / cheat_page_size);

  //
  // Should more-or-less match what's in: src/memory/smemory/generic.cpp
  //
  if((int)bSNES_v059::memory::cartram.size() > 0 && (bSNES_v059::memory::cartram.size() % cheat_page_size) == 0)
  {
   if(bSNES_v059::cartridge.mapper() == bSNES_v059::Cartridge::SuperFXROM)
   {

   }
   else if(bSNES_v059::cartridge.mapper() == bSNES_v059::Cartridge::SA1ROM)
   {
    CheatMap(true,  0x00, 0x3f, 0x3000, 0x37ff, bSNES_v059::memory::iram);    //cpuiram); 
    CheatMap(false, 0x00, 0x3f, 0x6000, 0x7fff, bSNES_v059::memory::cartram); //cc1bwram);
    CheatMap(true,  0x40, 0x4f, 0x0000, 0xffff, bSNES_v059::memory::cartram); //cc1bwram);
    CheatMap(false, 0x80, 0xbf, 0x3000, 0x37ff, bSNES_v059::memory::iram);    //cpuiram);
    CheatMap(false, 0x80, 0xbf, 0x6000, 0x7fff, bSNES_v059::memory::cartram); //cc1bwram);
   }
   else if(bSNES_v059::cartridge.mapper() == bSNES_v059::Cartridge::SPC7110ROM)
   {

   }
   else if(bSNES_v059::cartridge.mapper() == bSNES_v059::Cartridge::BSXROM)
   {

   }
   else if(bSNES_v059::cartridge.mapper() == bSNES_v059::Cartridge::BSCLoROM)
   {

   }
   else if(bSNES_v059::cartridge.mapper() == bSNES_v059::Cartridge::BSCHiROM)
   {

   }
   else if(bSNES_v059::cartridge.mapper() == bSNES_v059::Cartridge::STROM)
   {

   }
   else
   {
    if((int)bSNES_v059::memory::cartram.size() > 0)
    {
     CheatMap(false, 0x20, 0x3f, 0x6000, 0x7fff, bSNES_v059::memory::cartram);
     CheatMap(false, 0xa0, 0xbf, 0x6000, 0x7fff, bSNES_v059::memory::cartram);

     //research shows only games with very large ROM/RAM sizes require MAD-1 memory mapping of RAM
     //otherwise, default to safer, larger RAM address window
     uint16 addr_hi = (bSNES_v059::memory::cartrom.size() > 0x200000 || bSNES_v059::memory::cartram.size() > 32 * 1024) ? 0x7fff : 0xffff;
     const bool meowmeowmoocow = bSNES_v059::memory::cartram.size() <= ((addr_hi + 1) * 14) || bSNES_v059::cartridge.mapper() != bSNES_v059::Cartridge::LoROM;

     CheatMap(meowmeowmoocow, 0x70, 0x7f, 0x0000, addr_hi, bSNES_v059::memory::cartram);

     if(bSNES_v059::cartridge.mapper() == bSNES_v059::Cartridge::LoROM)
      CheatMap(!meowmeowmoocow, 0xf0, 0xff, 0x0000, addr_hi, bSNES_v059::memory::cartram);
    }
   }
  }

  //
  // System(WRAM) mappings should be done last, as they'll partially overwrite some of the cart mappings above, matching bsnes' internal semantics.
  //

  CheatMap(false, 0x00, 0x3f, 0x0000, 0x1fff, bSNES_v059::memory::wram, 0x000000, 0x002000);
  CheatMap(false, 0x80, 0xbf, 0x0000, 0x1fff, bSNES_v059::memory::wram, 0x000000, 0x002000);
  CheatMap(true,  0x7e, 0x7f, 0x0000, 0xffff, bSNES_v059::memory::wram);

  ColorMap.resize(32768);

  if(!LoadCPalette(NULL, &CustomColorMap, 32768))
  {
   Cleanup();
   return(0);
  }
 }
 catch(std::exception &e)
 {
  MDFND_PrintError(e.what());
  Cleanup();
  return 0;
 }

 return(1);
}

static void CloseGame(void)
{
 if(!snsf_loader)
 {
  SaveLoadMemory(false);
 }
 Cleanup();
}

static void Emulate(EmulateSpecStruct *espec)
{
 tsurf = espec->surface;
 tlw = espec->LineWidths;
 tdr = &espec->DisplayRect;
 es = espec;


 PrevLine = -1;

 if(!snsf_loader)
 {
  if(!espec->skip && tsurf && tlw)
  {
   tdr->x = 0;
   tdr->y = 0;
   tdr->w = 1;
   tdr->h = 1;
   tlw[0].w = 0;	// Mark line widths as valid(ie != ~0; since field == 1 would skip it).
  }

  if(espec->VideoFormatChanged)
   BuildColorMap(espec->surface->format);
 }

 if(SoundLastRate != espec->SoundRate)
 {
  if(resampler)
  {
   speex_resampler_destroy(resampler);
   resampler = NULL;
  }
  int err = 0;
  int quality = MDFN_GetSettingUI("snes.apu.resamp_quality");

  resampler = speex_resampler_init_frac(2, 64081, 2 * (int)(espec->SoundRate ? espec->SoundRate : 48000),
					   32040.5, (int)(espec->SoundRate ? espec->SoundRate : 48000), quality, &err);
  SoundLastRate = espec->SoundRate;

  //printf("%f ms\n", 1000.0 * speex_resampler_get_input_latency(resampler) / 32040.5);
 }

 if(!snsf_loader)
 {
  MDFNMP_ApplyPeriodicCheats();
 }

 // Make sure to trash any leftover samples, generated from system.runtosave() in save state saving, if sound is now disabled.
 if(SoundOn && !espec->SoundBuf)
 {
  ResampInPos = 0;
 }

 SoundOn = espec->SoundBuf ? true : false;

 HasPolledThisFrame = false;
 InProperEmu = TRUE;

 // More aggressive frameskipping disabled until we can rule out undesirable side-effects and interactions.
 //bSNES_v059::ppu.enable_renderer(!espec->skip || PrevFrameInterlaced);
 bSNES_v059::system.run_mednafen_custom();
 bSNES_v059::ppu.enable_renderer(true);


 //
 // Blank out any missed lines(for e.g. display height change with PAL emulation)
 //
 if(!snsf_loader && !es->skip && tsurf && tlw)
 {
  //printf("%d\n", PrevLine + 1);
  BlankMissingLines(PrevLine + 1, tdr->h >> es->InterlaceOn, es->InterlaceOn, es->InterlaceField);
 }

 tsurf = NULL;
 tlw = NULL;
 tdr = NULL;
 es = NULL;
 InProperEmu = FALSE;

 espec->MasterCycles = CycleCounter;
 CycleCounter = 0;

 //if(!espec->MasterCycles)
 //{
 // puts("BOGUS GNOMES");
 // espec->MasterCycles = 1;
 //}
 //printf("%d\n", espec->MasterCycles);

 if(espec->SoundBuf)
 {
  spx_uint32_t in_len; // "Number of input samples in the input buffer. Returns the number of samples processed. This is all per-channel."
  spx_uint32_t out_len; // "Size of the output buffer. Returns the number of samples written. This is all per-channel."

  //printf("%d\n", ResampInPos);
  in_len = ResampInPos;
  out_len = 524288; //8192;     // FIXME, real size.

  speex_resampler_process_interleaved_int(resampler, (const spx_int16_t *)ResampInBuffer, &in_len, (spx_int16_t *)espec->SoundBuf, &out_len);

  assert(in_len <= ResampInPos);

  if((ResampInPos - in_len) > 0)
   memmove(ResampInBuffer, ResampInBuffer + in_len, (ResampInPos - in_len) * sizeof(int16) * 2);

  ResampInPos -= in_len;

  espec->SoundBufSize = out_len;
 }

 MDFNGameInfo->mouse_sensitivity = MDFN_GetSettingF("snes.mouse_sensitivity");

 if(snsf_loader)
 {
  if(!espec->skip)
  {
   espec->LineWidths[0].w = ~0;
   Player_Draw(espec->surface, &espec->DisplayRect, 0, espec->SoundBuf, espec->SoundBufSize);
  }
 }


#if 0
 {
  static int skipframe = 3;

  if(skipframe)
   skipframe--;
  else
  {
   static unsigned fc = 0;
   static uint64 cc = 0;
   static uint64 cc2 = 0;

   fc++;
   cc += espec->MasterCycles;
   cc2 += espec->SoundBufSize;

   printf("%f %f\n", (double)fc / ((double)cc / 32040.5), (double)fc / ((double)cc2 / espec->SoundRate));
  }
 }
#endif
}

static int StateAction(StateMem *sm, int load, int data_only)
{
 const uint32 length = bSNES_v059::system.serialize_size();

 if(load)
 {
  uint8 *ptr;

  if(!(ptr = (uint8 *)MDFN_calloc(1, length, _("SNES save state buffer"))))
   return(0);

  SFORMAT StateRegs[] =
  {
   SFARRAYN(ptr, length, "OmniCat"),
   SFARRAY16(PadLatch, 8),
   SFARRAY16(MouseXLatch, 2),
   SFARRAY16(MouseYLatch, 2),
   SFARRAY(MouseBLatch, 2),
   SFEND
  };

  if(!MDFNSS_StateAction(sm, 1, data_only, StateRegs, "DATA"))
  {
   free(ptr);
   return(0);
  }

  //srand(99);
  //for(int i = 16; i < length; i++)
  // ptr[i] = rand() & 0x3;

  serializer state(ptr, length);
  int result;

  result = bSNES_v059::system.unserialize(state);

  free(ptr);
  return(result);
 }
 else // save:
 {
  if(bSNES_v059::scheduler.sync != bSNES_v059::Scheduler::SyncAll)
   bSNES_v059::system.runtosave();

  serializer state = bSNES_v059::system.serialize();

  assert(state.size() == length);

  uint8 *ptr = const_cast<uint8 *>(state.data());

  SFORMAT StateRegs[] =
  {
   SFARRAYN(ptr, length, "OmniCat"),
   SFARRAY16(PadLatch, 8),
   SFARRAY16(MouseXLatch, 2),
   SFARRAY16(MouseYLatch, 2),
   SFARRAY(MouseBLatch, 2),
   SFEND
  };

  if(!MDFNSS_StateAction(sm, 0, data_only, StateRegs, "DATA"))
   return(0);

  return(1);
 }

}

struct StrToBSIT_t
{
 const char *str;
 const int id;
};

static const StrToBSIT_t StrToBSIT[] =
{
 { "none",   	bSNES_v059::Input::DeviceNone },
 { "gamepad",   bSNES_v059::Input::DeviceJoypad },
 { "multitap",  bSNES_v059::Input::DeviceMultitap },
 { "mouse",   	bSNES_v059::Input::DeviceMouse },
 { "superscope",   bSNES_v059::Input::DeviceSuperScope },
 { "justifier",   bSNES_v059::Input::DeviceJustifier },
 { "justifiers",   bSNES_v059::Input::DeviceJustifiers },
 { NULL,	-1	},
};


static void SetInput(int port, const char *type, void *ptr)
{
 assert(port >= 0 && port < 8);

 if(port < 2)
 {
  const StrToBSIT_t *sb = StrToBSIT;
  int id = -1;

  if(MultitapEnabled[port] && !strcmp(type, "gamepad"))
   type = "multitap";

  while(sb->str && id == -1)
  {
   if(!strcmp(type, sb->str))
    id = sb->id;
   sb++;
  }
  assert(id != -1);

  InputType[port] = id;

  bSNES_v059::input.port_set_device(port, id);

#if 0
  switch(config().input.port1) { default:
    case ControllerPort1::None: mapper().port1 = 0; break;
    case ControllerPort1::Gamepad: mapper().port1 = &Controllers::gamepad1; break;
    case ControllerPort1::Asciipad: mapper().port1 = &Controllers::asciipad1; break;
    case ControllerPort1::Multitap: mapper().port1 = &Controllers::multitap1; break;
    case ControllerPort1::Mouse: mapper().port1 = &Controllers::mouse1; break;
  }

  switch(config().input.port2) { default:
    case ControllerPort2::None: mapper().port2 = 0; break;
    case ControllerPort2::Gamepad: mapper().port2 = &Controllers::gamepad2; break;
    case ControllerPort2::Asciipad: mapper().port2 = &Controllers::asciipad2; break;
    case ControllerPort2::Multitap: mapper().port2 = &Controllers::multitap2; break;
    case ControllerPort2::Mouse: mapper().port2 = &Controllers::mouse2; break;
    case ControllerPort2::SuperScope: mapper().port2 = &Controllers::superscope; break;
    case ControllerPort2::Justifier: mapper().port2 = &Controllers::justifier1; break;
    case ControllerPort2::Justifiers: mapper().port2 = &Controllers::justifiers; break;
  }
#endif

 }


 InputPtr[port] = (uint8 *)ptr;
}

static void SetLayerEnableMask(uint64 mask)
{

}


static void DoSimpleCommand(int cmd)
{
 switch(cmd)
 {
  case MDFN_MSC_RESET: bSNES_v059::system.reset(); break;
  case MDFN_MSC_POWER: bSNES_v059::system.power(); break;
 }
}

static const InputDeviceInputInfoStruct GamepadIDII[] =
{
 { "b", "B (center, lower)", 7, IDIT_BUTTON_CAN_RAPID, NULL },
 { "y", "Y (left)", 6, IDIT_BUTTON_CAN_RAPID, NULL },
 { "select", "SELECT", 4, IDIT_BUTTON, NULL },
 { "start", "START", 5, IDIT_BUTTON, NULL },
 { "up", "UP ↑", 0, IDIT_BUTTON, "down" },
 { "down", "DOWN ↓", 1, IDIT_BUTTON, "up" },
 { "left", "LEFT ←", 2, IDIT_BUTTON, "right" },
 { "right", "RIGHT →", 3, IDIT_BUTTON, "left" },
 { "a", "A (right)", 9, IDIT_BUTTON_CAN_RAPID, NULL },
 { "x", "X (center, upper)", 8, IDIT_BUTTON_CAN_RAPID, NULL },
 { "l", "Left Shoulder", 10, IDIT_BUTTON, NULL },
 { "r", "Right Shoulder", 11, IDIT_BUTTON, NULL },
};

static const InputDeviceInputInfoStruct MouseIDII[0x4] =
{
 { "x_axis", "X Axis", -1, IDIT_X_AXIS_REL },
 { "y_axis", "Y Axis", -1, IDIT_Y_AXIS_REL },
 { "left", "Left Button", 0, IDIT_BUTTON, NULL },
 { "right", "Right Button", 1, IDIT_BUTTON, NULL },
};

#if 0
static const InputDeviceInputInfoStruct SuperScopeIDII[] =
{

};
#endif

static InputDeviceInfoStruct InputDeviceInfoSNESPort[] =
{
 // None
 {
  "none",
  "none",
  NULL,
  NULL,
  0,
  NULL
 },

 // Gamepad
 {
  "gamepad",
  "Gamepad",
  NULL,
  NULL,
  sizeof(GamepadIDII) / sizeof(InputDeviceInputInfoStruct),
  GamepadIDII,
 },

 // Mouse
 {
  "mouse",
  "Mouse",
  NULL,
  NULL,
  sizeof(MouseIDII) / sizeof(InputDeviceInputInfoStruct),
  MouseIDII,
 },
#if 0
 {
  "superscope",
  "Super Scope",
  gettext_noop("Monkey!"),
  NULL,
  sizeof(SuperScopeIDII) / sizeof(InputDeviceInputInfoStruct),
  SuperScopeIDII
 },
#endif
};


static InputDeviceInfoStruct InputDeviceInfoTapPort[] =
{
 // Gamepad
 {
  "gamepad",
  "Gamepad",
  NULL,
  NULL,
  sizeof(GamepadIDII) / sizeof(InputDeviceInputInfoStruct),
  GamepadIDII,
 },
};


static const InputPortInfoStruct PortInfo[] =
{
 { "port1", "Port 1/1A", sizeof(InputDeviceInfoSNESPort) / sizeof(InputDeviceInfoStruct), InputDeviceInfoSNESPort, "gamepad" },
 { "port2", "Port 2/2A", sizeof(InputDeviceInfoSNESPort) / sizeof(InputDeviceInfoStruct), InputDeviceInfoSNESPort, "gamepad" },
 { "port3", "Port 2B", sizeof(InputDeviceInfoTapPort) / sizeof(InputDeviceInfoStruct), InputDeviceInfoTapPort, "gamepad" },
 { "port4", "Port 2C", sizeof(InputDeviceInfoTapPort) / sizeof(InputDeviceInfoStruct), InputDeviceInfoTapPort, "gamepad" },
 { "port5", "Port 2D", sizeof(InputDeviceInfoTapPort) / sizeof(InputDeviceInfoStruct), InputDeviceInfoTapPort, "gamepad" },
 { "port6", "Port 1B", sizeof(InputDeviceInfoTapPort) / sizeof(InputDeviceInfoStruct), InputDeviceInfoTapPort, "gamepad" },
 { "port7", "Port 1C", sizeof(InputDeviceInfoTapPort) / sizeof(InputDeviceInfoStruct), InputDeviceInfoTapPort, "gamepad" },
 { "port8", "Port 1D", sizeof(InputDeviceInfoTapPort) / sizeof(InputDeviceInfoStruct), InputDeviceInfoTapPort, "gamepad" },
};

static InputInfoStruct SNESInputInfo =
{
 sizeof(PortInfo) / sizeof(InputPortInfoStruct),
 PortInfo
};

static void InstallReadPatch(uint32 address, uint8 value, int compare)
{
 bSNES_v059::CheatCode tc;

 tc.addr = address;
 tc.data = value;
 tc.compare = compare;

 //printf("%08x %02x %d\n", address, value, compare);

 bSNES_v059::cheat.enable(true);
 bSNES_v059::cheat.install_read_patch(tc);
}

static void RemoveReadPatches(void)
{
 bSNES_v059::cheat.enable(false);
 bSNES_v059::cheat.remove_read_patches();
}


static const MDFNSetting SNESSettings[] =
{
 { "snes.input.port1.multitap", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, gettext_noop("Enable multitap on SNES port 1."), NULL, MDFNST_BOOL, "0", NULL, NULL },
 { "snes.input.port2.multitap", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, gettext_noop("Enable multitap on SNES port 2."), NULL, MDFNST_BOOL, "0", NULL, NULL },

 { "snes.mouse_sensitivity", MDFNSF_NOFLAGS, gettext_noop("Emulated mouse sensitivity."), NULL, MDFNST_FLOAT, "0.50", NULL, NULL, NULL },

 { "snes.correct_aspect", MDFNSF_CAT_VIDEO, gettext_noop("Correct the aspect ratio."), gettext_noop("Note that regardless of this setting's value, \"512\" and \"256\" width modes will be scaled to the same dimensions for display."), MDFNST_BOOL, "0" },

 { "snes.apu.resamp_quality", MDFNSF_NOFLAGS, gettext_noop("APU output resampler quality."), gettext_noop("0 is lowest quality and latency and CPU usage, 10 is highest quality and latency and CPU usage.\n\nWith a Mednafen sound output rate of about 32041Hz or higher: Quality \"0\" resampler has approximately 0.125ms of latency, quality \"5\" resampler has approximately 1.25ms of latency, and quality \"10\" resampler has approximately 3.99ms of latency."), MDFNST_UINT, "5", "0", "10" },

 { NULL }
};


static bool DecodeGG(const std::string& cheat_string, MemoryPatch* patch)
{
 if(cheat_string.size() != 8 && cheat_string.size() != 9)
  throw MDFN_Error(0, _("Game Genie code is of an incorrect length."));

 if(cheat_string.size() == 9 && (cheat_string[4] != '-' && cheat_string[4] != '_' && cheat_string[4] != ' '))
  throw MDFN_Error(0, _("Game Genie code is malformed."));

 uint32 ev = 0;

 for(unsigned i = 0; i < 8; i++)
 {
  int c = cheat_string[(i >= 4 && cheat_string.size() == 9) ? (i + 1) : i];
  static const uint8 subst_table[16] = { 0x4, 0x6, 0xD, 0xE, 0x2, 0x7, 0x8, 0x3,
				  	 0xB, 0x5, 0xC, 0x9, 0xA, 0x0, 0xF, 0x1 };
  ev <<= 4;

  if(c >= '0' && c <= '9')
   ev |= subst_table[c - '0'];
  else if(c >= 'a' && c <= 'f')
   ev |= subst_table[c - 'a' + 0xA];
  else if(c >= 'A' && c <= 'F')
   ev |= subst_table[c - 'A' + 0xA];
  else
  {
   if(c & 0x80)
    throw MDFN_Error(0, _("Invalid character in Game Genie code."));
   else
    throw MDFN_Error(0, _("Invalid character in Game Genie code: %c"), c);
  }
 }

 uint32 addr = 0;
 uint8 val = 0;;
 static const uint8 bm[24] = 
 {
  0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x0e, 0x0f, 0x00, 0x01, 0x14, 0x15, 0x16, 0x17, 0x02, 0x03, 0x04, 0x05, 0x0a, 0x0b, 0x0c, 0x0d
 };


 val = ev >> 24;
 for(unsigned i = 0; i < 24; i++)
  addr |= ((ev >> bm[i]) & 1) << i;

 patch->addr = addr;
 patch->val = val;
 patch->length = 1;
 patch->type = 'S';

 //printf("%08x %02x\n", addr, val);

 return(false);
}

static bool DecodePAR(const std::string& cheat_string, MemoryPatch* patch)
{
 uint32 addr;
 uint8 val;

 if(cheat_string.size() != 8 && cheat_string.size() != 9)
  throw MDFN_Error(0, _("Pro Action Replay code is of an incorrect length."));

 if(cheat_string.size() == 9 && (cheat_string[6] != ':' && cheat_string[6] != ';' && cheat_string[6] != ' '))
  throw MDFN_Error(0, _("Pro Action Replay code is malformed."));


 uint32 ev = 0;

 for(unsigned i = 0; i < 8; i++)
 {
  int c = cheat_string[(i >= 6 && cheat_string.size() == 9) ? (i + 1) : i];

  ev <<= 4;

  if(c >= '0' && c <= '9')
   ev |= c - '0';
  else if(c >= 'a' && c <= 'f')
   ev |= c - 'a' + 0xA;
  else if(c >= 'A' && c <= 'F')
   ev |= c - 'A' + 0xA;
  else
  {
   if(c & 0x80)
    throw MDFN_Error(0, _("Invalid character in Pro Action Replay code."));
   else
    throw MDFN_Error(0, _("Invalid character in Pro Action Replay code: %c"), c);
  }
 }

 patch->addr = ev >> 8;
 patch->val = ev & 0xFF;
 patch->length = 1;
 patch->type = 'R';

 return(false);
}

static CheatFormatStruct CheatFormats[] =
{
 { "Game Genie", "", DecodeGG },
 { "Pro Action Replay", "", DecodePAR },
};

static CheatFormatInfoStruct CheatFormatInfo =
{
 2,
 CheatFormats
};


static const FileExtensionSpecStruct KnownExtensions[] =
{
 { ".smc", "Super Magicom ROM Image" },
 { ".swc", "Super Wildcard ROM Image" },
 { ".sfc", "Cartridge ROM Image" },
 { ".fig", "Cartridge ROM Image" },

 { ".bs", "BS-X EEPROM Image" },
 { ".st", "Sufami Turbo Cartridge ROM Image" },

 { NULL, NULL }
};

MDFNGI EmulatedSNES =
{
 "snes",
 "Super Nintendo Entertainment System/Super Famicom",
 KnownExtensions,
 MODPRIO_INTERNAL_HIGH,
 NULL,						// Debugger
 &SNESInputInfo,
 Load,
 TestMagic,
 NULL,
 NULL,
 CloseGame,
 SetLayerEnableMask,
 NULL,	// Layer names, null-delimited
 NULL,
 NULL,
 InstallReadPatch,
 RemoveReadPatches,
 NULL, //MemRead,
 &CheatFormatInfo,
 true,
 StateAction,
 Emulate,
 SetInput,
 DoSimpleCommand,
 SNESSettings,
 0,
 0,
 FALSE, // Multires

 512,   // lcm_width
 480,   // lcm_height           (replaced in game load)
 NULL,  // Dummy

 256,   // Nominal width	(replaced in game load)
 240,   // Nominal height	(replaced in game load)
 
 512,	// Framebuffer width
 512,	// Framebuffer height

 2,     // Number of output sound channels
};


