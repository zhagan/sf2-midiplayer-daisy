#include "sd_mount.h"
#include "daisy_patch_sm.h"

extern "C"
{
#include "ff.h"
}

using namespace daisy;

static SdmmcHandler   s_sdmmc;
static FatFSInterface s_fsi;
static FATFS*         s_fs = nullptr;

bool SdMount()
{
    SdmmcHandler::Config sd_cfg;
    sd_cfg.Defaults();
    sd_cfg.speed = SdmmcHandler::Speed::STANDARD;
    s_sdmmc.Init(sd_cfg);

    FatFSInterface::Config fsi_cfg;
    fsi_cfg.media = FatFSInterface::Config::MEDIA_SD;
    s_fsi.Init(fsi_cfg);

    s_fs = &s_fsi.GetSDFileSystem();

    const FRESULT mnt = f_mount(s_fs, "0:", 1);
    return (mnt == FR_OK);
}
