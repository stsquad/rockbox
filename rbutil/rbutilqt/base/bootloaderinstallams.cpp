/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 *   Copyright (C) 2008 by Dominik Wenger
 *   $Id$
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <QtCore>
#include "bootloaderinstallbase.h"
#include "bootloaderinstallams.h"

extern "C"
{
    #include "../mkamsboot/mkamsboot.h"
};

BootloaderInstallAms::BootloaderInstallAms(QObject *parent)
        : BootloaderInstallBase(parent)
{
}

QString BootloaderInstallAms::ofHint()
{
    return tr("Bootloader installation requires you to provide "
               "a firmware file of the original firmware (bin file). "
               "You need to download this file yourself due to legal "
               "reasons." 
               "Press Ok to continue and browse your computer for the firmware "
               "file.");
}

bool BootloaderInstallAms::install(void)
{
    if(m_offile.isEmpty())
        return false;
    
    // download firmware from server
    emit logItem(tr("Downloading bootloader file"), LOGINFO);
    
    connect(this, SIGNAL(downloadDone()), this, SLOT(installStage2()));
    downloadBlStart(m_blurl);
    
    return true;
}

void BootloaderInstallAms::installStage2(void)
{    
    unsigned char* buf;
    unsigned char* of_packed;
    int of_packedsize;
    unsigned char* rb_packed;
    int rb_packedsize;
    off_t len;
    struct md5sums sum;
    char md5sum[33]; /* 32 hex digits, plus terminating zero */
    int n;
    int firmware_size;
    int bootloader_size;
    int totalsize;
    char errstr[200];
      
    sum.md5 = md5sum;  
      
    m_tempfile.open();
    QString bootfile = m_tempfile.fileName();
    m_tempfile.close();

    /* Load original firmware file */                
    buf = load_of_file(m_offile.toLocal8Bit().data(), &len,&sum,&firmware_size,
                        &of_packed,&of_packedsize,errstr,sizeof(errstr));
    if (buf == NULL) 
    {
        emit logItem(errstr, LOGERROR);
        emit logItem(tr("Could not load %1").arg(m_offile), LOGERROR);
        emit done(true);
        return;
    }

    /* Load bootloader file */
    rb_packed = load_rockbox_file(bootfile.toLocal8Bit().data(), sum.model, &bootloader_size,&rb_packedsize,
                                    errstr,sizeof(errstr));
    if (rb_packed == NULL) 
    {
        emit logItem(errstr, LOGERROR);
        emit logItem(tr("Could not load %1").arg(bootfile), LOGERROR);
        free(buf);
        free(of_packed);
        emit done(true);
        return;
    }
    
    /* check total size */
    totalsize = total_size(sum.model,rb_packedsize,of_packedsize);
    if (totalsize > firmware_size) 
    {
        emit logItem("No room to insert bootloader, try another firmware version",LOGERROR);
        free(buf);
        free(of_packed);
        free(rb_packed);
        emit done(true);
        return;
    }
    
    /* patch the firmware */
    emit logItem(tr("Patching Firmware..."), LOGINFO);
    
    patch_firmware(sum.model,firmware_revision(sum.model),firmware_size,buf,len,of_packed,of_packedsize,rb_packed,rb_packedsize);

    /* construct path for write out. combine path of m_blfile with filename from of */
    QString outfilename = QFileInfo(m_blfile).absolutePath() + "/" +QFileInfo(m_offile).fileName();
    /* write out file*/ 
    QFile out(outfilename); 
    
    if(!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        emit logItem(tr("Could not open %1 for writing").arg(m_blfile),LOGERROR);
        free(buf);
        free(of_packed);
        free(rb_packed);
        emit done(true);
        return;
    }
    
    n = out.write((char*)buf, len);

    if (n != len)
    {
        emit logItem(tr("Could not write firmware file"),LOGERROR);
        free(buf);
        free(of_packed);
        free(rb_packed);
        emit done(true);
        return;
    }

    out.close();
    
    free(buf);
    free(of_packed);
    free(rb_packed);
    
    //end of install
    emit logItem(tr("Success: modified firmware file created"), LOGINFO);
    logInstall(LogAdd);
    emit done(false);
    return;
}

bool BootloaderInstallAms::uninstall(void)
{
    emit logItem("To uninstall, perform a normal upgrade with an unmodified original firmware", LOGINFO);
    logInstall(LogRemove);
    return false;
}

BootloaderInstallBase::BootloaderType BootloaderInstallAms::installed(void)
{
    return BootloaderUnknown;
}

BootloaderInstallBase::Capabilities BootloaderInstallAms::capabilities(void)
{
    return (Install | NeedsOf);
}
