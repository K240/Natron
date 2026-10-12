/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "AppManagerPrivate.h"

#include <QtGlobal> // for Q_OS_*
#if defined(Q_OS_UNIX)
#include <sys/time.h>     // for getrlimit on linux
#include <sys/resource.h> // for getrlimit
#if defined(__APPLE__)
#include <sys/syslimits.h> // OPEN_MAX
#endif
#endif
#include <cstddef>
#include <cstdlib>
#include <cassert>
#include <stdexcept>
#include <sstream> // stringstream

GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
#include <boost/serialization/export.hpp>
#include <boost/archive/binary_oarchive.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
// clang-format on

#include <QDebug>
#include <QProcess>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>

#include "Global/QtCompat.h" // for removeRecursively
#include "Global/GlobalDefines.h"
#include "Global/ProcInfo.h"
#include "Global/StrUtils.h"
#include "Global/FStreamsSupport.h"

#include "Engine/CacheSerialization.h"
#include "Engine/CLArgs.h"
#include "Engine/ExistenceCheckThread.h"
#include "Engine/Format.h"
#include "Engine/FrameEntry.h"
#include "Engine/Image.h"
#include "Engine/OfxHost.h"
#include "Engine/ProcessHandler.h" // ProcessInputChannel
#include "Engine/RectDSerialization.h"
#include "Engine/RectISerialization.h"
#include "Engine/StandardPaths.h"


BOOST_CLASS_EXPORT(NATRON_NAMESPACE::FrameParams)
BOOST_CLASS_EXPORT(NATRON_NAMESPACE::ImageParams)

//Borrowed from https://github.com/python/cpython/blob/634cb7aa2936a09e84c5787d311291f0e042dba3/Python/fileutils.c
//Somehow Python 3 dev forced every C application embedding python to have their own code to convert char** to wchar_t**
static wchar_t*
char2wchar(const char* arg)
{
    wchar_t *res = NULL;

#ifdef HAVE_BROKEN_MBSTOWCS
    /* Some platforms have a broken implementation of
     * mbstowcs which does not count the characters that
     * would result from conversion.  Use an upper bound.
     */
    size_t argsize = strlen(arg);
#else
    size_t argsize = mbstowcs(NULL, arg, 0);
#endif

    size_t count;
    unsigned char *in;
    wchar_t *out;
#ifdef HAVE_MBRTOWC
    mbstate_t mbs;
#endif
    if (argsize != (size_t)-1) {
        res = (wchar_t *)malloc( (argsize + 1) * sizeof(wchar_t) );
        if (!res) {
            goto oom;
        }
        count = mbstowcs(res, arg, argsize + 1);
        if (count != (size_t)-1) {
            wchar_t *tmp;
            /* Only use the result if it contains no
             surrogate characters. */
            for (tmp = res; *tmp != 0 &&
                 (*tmp < 0xd800 || *tmp > 0xdfff); tmp++) {
                ;
            }
            if (*tmp == 0) {
                return res;
            }
        }
        free(res);
    }
    /* Conversion failed. Fall back to escaping with surrogateescape. */
#ifdef HAVE_MBRTOWC
    /* Try conversion with mbrtwoc (C99), and escape non-decodable bytes. */
    /* Overallocate; as multi-byte characters are in the argument, the
     actual output could use less memory. */
    argsize = strlen(arg) + 1;
    res = (wchar_t*)malloc( argsize * sizeof(wchar_t) );
    if (!res) {
        goto oom;
    }
    in = (unsigned char*)arg;
    out = res;
    std::memset(&mbs, 0, sizeof mbs);
    while (argsize) {
        size_t converted = mbrtowc(out, (char*)in, argsize, &mbs);
        if (converted == 0) {
            /* Reached end of string; null char stored. */
            break;
        }
        if (converted == (size_t)-2) {
            /* Incomplete character. This should never happen,
             since we provide everything that we have -
             unless there is a bug in the C library, or I
             misunderstood how mbrtowc works. */
            fprintf(stderr, "unexpected mbrtowc result -2\n");
            free(res);

            return NULL;
        }
        if (converted == (size_t)-1) {
            /* Conversion error. Escape as UTF-8b, and start over
             in the initial shift state. */
            *out++ = 0xdc00 + *in++;
            argsize--;
            std::memset(&mbs, 0, sizeof mbs);
            continue;
        }
        if ( (*out >= 0xd800) && (*out <= 0xdfff) ) {
            /* Surrogate character.  Escape the original
             byte sequence with surrogateescape. */
            argsize -= converted;
            while (converted--) {
                *out++ = 0xdc00 + *in++;
            }
            continue;
        }
        /* successfully converted some bytes */
        in += converted;
        argsize -= converted;
        out++;
    }
#else
    /* Cannot use C locale for escaping; manually escape as if charset
     is ASCII (i.e. escape all bytes > 128. This will still roundtrip
     correctly in the locale's charset, which must be an ASCII superset. */
    res = (wchar_t*)malloc( (strlen(arg) + 1) * sizeof(wchar_t) );
    if (!res) {
        goto oom;
    }
    in = (unsigned char*)arg;
    out = res;
    while (*in) {
        if (*in < 128) {
            *out++ = *in++;
        } else {
            *out++ = 0xdc00 + *in++;
        }
    }
    *out = 0;
#endif // ifdef HAVE_MBRTOWC

    return res;
oom:
    fprintf(stderr, "out of memory\n");
    free(res);

    return NULL;
} // char2wchar

NATRON_NAMESPACE_ENTER
AppManagerPrivate::AppManagerPrivate()
    : globalTLS()
    , _appType(AppManager::eAppTypeBackground)
    , _appInstancesMutex()
    , _appInstances()
    , _availableID(0)
    , _topLevelInstanceID(0)
    , _settings()
    , _formats()
    , _plugins()
    , readerPlugins()
    , writerPlugins()
    , ofxHost( new OfxHost() )
    , _knobFactory( new KnobFactory() )
    , _nodeCache()
    , _diskCache()
    , _viewerCache()
    , diskCachesLocationMutex()
    , diskCachesLocation()
    , _backgroundIPC()
    , _loaded(false)
    , _binaryPath()
    , _nodesGlobalMemoryUse(0)
    , errorLogMutex()
    , errorLog()
    , maxCacheFiles(0)
    , currentCacheFilesCount(0)
    , currentCacheFilesCountMutex()
    , idealThreadCount(0)
    , nThreadsToRender(0)
    , nThreadsPerEffect(0)
    , useThreadPool(true)
    , nThreadsMutex()
    , runningThreadsCount()
    , lastProjectLoadedCreatedDuringRC2Or3(false)
    , commandLineArgsWideOriginal()
    , commandLineArgsWide()
    , commandLineArgsUtf8Original()
    , commandLineArgsUtf8()
    , nArgs(0)
    , mainModule(0)
    , mainThreadState(0)
#ifdef NATRON_USE_BREAKPAD
    , breakpadProcessExecutableFilePath()
    , breakpadProcessPID(0)
    , breakpadHandler()
    , breakpadAliveThread()
#endif
#ifdef USE_NATRON_GIL
    , natronPythonGIL(QMutex::Recursive)
#endif
    , pluginsUseInputImageCopyToRender(false)
{
    setMaxCacheFiles();

    runningThreadsCount = 0;

#if defined(__NATRON_LINUX__)
    onWayland = qEnvironmentVariableIsSet("WAYLAND_DISPLAY") && !qEnvironmentVariableIsSet("NATRON_DISABLE_WAYLAND");
#endif
}

AppManagerPrivate::~AppManagerPrivate()
{


    for (std::size_t i = 0; i < commandLineArgsUtf8Original.size(); ++i) {
        free(commandLineArgsUtf8Original[i]);
    }
    for (std::size_t i = 0; i < commandLineArgsWideOriginal.size(); ++i) {
        free(commandLineArgsWideOriginal[i]);
    }
}

#ifdef NATRON_USE_BREAKPAD
void
AppManagerPrivate::initBreakpad(const QString& breakpadPipePath,
                                const QString& breakpadComPipePath,
                                int breakpad_client_fd)
{
    assert(!breakpadHandler);
    createBreakpadHandler(breakpadPipePath, breakpad_client_fd);

    /*
       We check periodically that the crash reporter process is still alive. If the user killed it somehow, then we want
       the Natron process to terminate
     */
    breakpadAliveThread = std::make_shared<ExistenceCheckerThread>(QString::fromUtf8(NATRON_NATRON_TO_BREAKPAD_EXISTENCE_CHECK),
                                                                     QString::fromUtf8(NATRON_NATRON_TO_BREAKPAD_EXISTENCE_CHECK_ACK),
                                                                     breakpadComPipePath);
    QObject::connect( breakpadAliveThread.get(), SIGNAL(otherProcessUnreachable()), appPTR, SLOT(onCrashReporterNoLongerResponding()) );
    breakpadAliveThread->start();
}

void
AppManagerPrivate::createBreakpadHandler(const QString& breakpadPipePath,
                                         int breakpad_client_fd)
{
    QString dumpPath = StandardPaths::writableLocation(StandardPaths::eStandardLocationTemp);

    Q_UNUSED(breakpad_client_fd);
    try {
#if defined(Q_OS_DARWIN)
        Q_UNUSED(breakpad_client_fd);
        breakpadHandler = std::make_shared<google_breakpad::ExceptionHandler>( dumpPath.toStdString(),
                                                                                 google_breakpad::ExceptionHandler::FilterCallback(NULL),
                                                                                 google_breakpad::ExceptionHandler::MinidumpCallback(NULL) /*dmpcb*/,
                                                                                 (void*)NULL,
                                                                                 true,
                                                                                 breakpadPipePath.toStdString().c_str() );
#elif defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
        Q_UNUSED(breakpadPipePath);
        breakpadHandler = std::make_shared<google_breakpad::ExceptionHandler>( google_breakpad::MinidumpDescriptor( dumpPath.toStdString() ),
                                                                                 google_breakpad::ExceptionHandler::FilterCallback(NULL),
                                                                                 google_breakpad::ExceptionHandler::MinidumpCallback(NULL) /*dmpCb*/,
                                                                                 (void*)NULL,
                                                                                 true,
                                                                                 breakpad_client_fd);
#elif defined(Q_OS_WIN32)
        Q_UNUSED(breakpad_client_fd);
        breakpadHandler = std::make_shared<google_breakpad::ExceptionHandler>( dumpPath.toStdWString(),
                                                                                 google_breakpad::ExceptionHandler::FilterCallback(NULL), //filter callback
                                                                                 google_breakpad::ExceptionHandler::MinidumpCallback(NULL) /*dmpcb*/,
                                                                                 (void*)NULL, //context
                                                                                 google_breakpad::ExceptionHandler::HANDLER_ALL,
                                                                                 MiniDumpNormal,
                                                                                 breakpadPipePath.toStdWString().c_str(),
                                                                                 (google_breakpad::CustomClientInfo*)NULL);
#endif
    } catch (const std::exception& e) {
        qDebug() << e.what();

        return;
    }
}

#endif // NATRON_USE_BREAKPAD


void
AppManagerPrivate::initProcessInputChannel(const QString & mainProcessServerName)
{
    // scoped_ptr
    _backgroundIPC.reset( new ProcessInputChannel(mainProcessServerName) );
}

void
AppManagerPrivate::loadBuiltinFormats()
{
    // Initializing list of all Formats available by default in a project, the user may add formats to this list via Python in the init.py script, see
    // example here: http://natron.readthedocs.io/en/workshop/startupscripts.html#init-py
    std::vector<std::string> formatNames;
    struct BuiltinFormat
    {
        const char *name;
        int w;
        int h;
        double par;
    };

    // list of formats: the names must match those in ofxsFormatResolution.h
    BuiltinFormat formats[] = {
        { "PC_Video",              640,  480, 1 },
        { "NTSC",                  720,  486, 0.91 },
        { "PAL",                   720,  576, 1.09 },
        { "NTSC_16:9",             720,  486, 1.21 },
        { "PAL_16:9",              720,  576, 1.46 },
        { "HD_720",               1280,  720, 1 },
        { "HD",                   1920, 1080, 1 },
        { "UHD_4K",               3840, 2160, 1 },
        { "1K_Super_35(full-ap)", 1024,  778, 1 },
        { "1K_Cinemascope",        914,  778, 2 },
        { "2K_Super_35(full-ap)", 2048, 1556, 1 },
        { "2K_Cinemascope",       1828, 1556, 2 },
        { "2K_DCP",               2048, 1080, 1 },
        { "4K_Super_35(full-ap)", 4096, 3112, 1 },
        { "4K_Cinemascope",       3656, 3112, 2 },
        { "4K_DCP",               4096, 2160, 1 },
        { "square_256",            256,  256, 1 },
        { "square_512",            512,  512, 1 },
        { "square_1K",            1024, 1024, 1 },
        { "square_2K",            2048, 2048, 1 },
        { NULL, 0, 0, 0 }
    };

    for (BuiltinFormat* f = &formats[0]; f->name != NULL; ++f) {
        Format _frmt(0, 0, f->w, f->h, f->name, f->par);
        _formats.push_back(_frmt);
    }
} // loadBuiltinFormats

//
// only used for Natron internal plugins (ViewerGroup, Dot, DiskCache, BackDrop, Roto)
// see also AppManager::getPluginBinary(), OFX::Host::PluginCache::getPluginById()
//
Plugin*
AppManagerPrivate::findPluginById(const QString& newId,
                                  int major,
                                  int minor) const
{
    for (PluginsMap::const_iterator it = _plugins.begin(); it != _plugins.end(); ++it) {
        for (PluginVersionsOrdered::const_reverse_iterator itver = it->second.rbegin();
             itver != it->second.rend();
             ++itver) {
            if ( ( (*itver)->getPluginID() == newId ) &&
                 ( (*itver)->getMajorVersion() == major ) &&
                 ( (*itver)->getMinorVersion() == minor ) ) {
                return (*itver);
            }
        }
    }

    return 0;
}

void
AppManagerPrivate::declareSettingsToPython()
{
    std::stringstream ss;

    ss <<  NATRON_ENGINE_PYTHON_MODULE_NAME << ".natron.settings = " << NATRON_ENGINE_PYTHON_MODULE_NAME << ".natron.getSettings()\n";
    const KnobsVec& knobs = _settings->getKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        const std::string name = (*it)->getName();
        if ( NATRON_PYTHON_NAMESPACE::isKeyword(name) ) {
            throw std::runtime_error(name + " is a Python keyword");
        }
        ss <<  NATRON_ENGINE_PYTHON_MODULE_NAME << ".natron.settings." <<
        name << " = " << NATRON_ENGINE_PYTHON_MODULE_NAME << ".natron.settings.getParam('" << name << "')\n";
    }
}

template <typename T>
void
saveCache(Cache<T>* cache)
{
    if (!cache) {
        return;
    }

    std::string cacheRestoreFilePath = cache->getRestoreFilePath();
    FStreamsSupport::ofstream ofile;
    FStreamsSupport::open(&ofile, cacheRestoreFilePath);

    if (!ofile) {
        std::cerr << "Failed to save cache to " << cacheRestoreFilePath.c_str() << std::endl;

        return;
    }


    typename Cache<T>::CacheTOC toc;
    cache->save(&toc);
    unsigned int version = cache->cacheVersion();
    try {
        boost::archive::binary_oarchive oArchive(ofile);
        oArchive << version;
        oArchive << toc;
    } catch (const std::exception & e) {
        qDebug() << "Failed to serialize the cache table of contents:" << e.what();
    }
}

void
AppManagerPrivate::saveCaches()
{
    if (!appPTR->isBackground()) {
        saveCache<FrameEntry>( _viewerCache.get() );
    }
    saveCache<Image>( _diskCache.get() );
} // saveCaches

template <typename T>
void
restoreCache(AppManagerPrivate* p,
             Cache<T>* cache)
{
    if ( p->checkForCacheDiskStructure( cache->getCachePath(), cache->isTileCache() ) ) {
        std::string settingsFilePath = cache->getRestoreFilePath();
        FStreamsSupport::ifstream ifile;
        FStreamsSupport::open(&ifile, settingsFilePath);
        if (!ifile) {
            std::cerr << "Failure to open cache restore file at: " << settingsFilePath << std::endl;

            return;
        }
        typename Cache<T>::CacheTOC tableOfContents;
        unsigned int cacheVersion = 0x1; //< default to 1 before NATRON_CACHE_VERSION was introduced
        try {
            boost::archive::binary_iarchive iArchive(ifile);
            if (cache->cacheVersion() >= NATRON_CACHE_VERSION) {
                iArchive >> cacheVersion;
            }
            //Only load caches with same version, otherwise wipe it!
            if ( cacheVersion == cache->cacheVersion() ) {
                iArchive >> tableOfContents;
            } else {
                p->cleanUpCacheDiskStructure( cache->getCachePath(), cache->isTileCache() );
            }
        } catch (const std::exception & e) {
            qDebug() << "Exception when reading disk cache TOC:" << e.what();
            p->cleanUpCacheDiskStructure( cache->getCachePath(), cache->isTileCache() );

            return;
        }

        QFile restoreFile( QString::fromUtf8( settingsFilePath.c_str() ) );
        restoreFile.remove();

        cache->restore(tableOfContents);
    }
}

void
AppManagerPrivate::restoreCaches()
{
    restoreCache<FrameEntry>( this, _viewerCache.get() );
    restoreCache<Image>( this, _diskCache.get() );
} // restoreCaches

bool
AppManagerPrivate::checkForCacheDiskStructure(const QString & cachePath, bool isTiled)
{
    QString settingsFilePath = cachePath;

    if ( !settingsFilePath.endsWith( QChar::fromLatin1('/') ) ) {
        settingsFilePath += QChar::fromLatin1('/');
    }
    settingsFilePath += QString::fromUtf8("restoreFile." NATRON_CACHE_FILE_EXT);

    if ( !QFile::exists(settingsFilePath) ) {
        cleanUpCacheDiskStructure(cachePath, isTiled);

        return false;
    }

    if (!isTiled) {
        QDir directory(cachePath);
        QStringList files = directory.entryList(QDir::AllDirs);


        /*Now counting actual data files in the cache*/
        /*check if there's 256 subfolders, otherwise reset cache.*/
        int count = 0; // -1 because of the restoreFile
        int subFolderCount = 0;
        Q_FOREACH(const QString &file, files) {
            QString subFolder(cachePath);

            subFolder.append( QDir::separator() );
            subFolder.append(file);
            if ( ( subFolder.right(1) == QString::fromUtf8(".") ) || ( subFolder.right(2) == QString::fromUtf8("..") ) ) {
                continue;
            }
            QDir d(subFolder);
            if ( d.exists() ) {
                ++subFolderCount;
                QStringList items = d.entryList();
                for (int j = 0; j < items.size(); ++j) {
                    if ( ( items[j] != QString::fromUtf8(".") ) && ( items[j] != QString::fromUtf8("..") ) ) {
                        ++count;
                    }
                }
            }
        }
        if (subFolderCount < 256) {
            qDebug() << cachePath << "doesn't contain sub-folders indexed from 00 to FF. Resetting.";
            cleanUpCacheDiskStructure(cachePath, isTiled);

            return false;
        }
    }
    return true;
} // AppManagerPrivate::checkForCacheDiskStructure

void
AppManagerPrivate::cleanUpCacheDiskStructure(const QString & cachePath, bool isTiled)
{
    /*re-create cache*/

    QDir cacheFolder(cachePath);

    if ( cacheFolder.exists() ) {
        cacheFolder.removeRecursively();
    }

    if ( !cacheFolder.exists() ) {
        bool success = cacheFolder.mkpath( QChar::fromLatin1('.') );
        if (!success) {
            qDebug() << "Warning: cache directory" << cachePath << "could not be created";
        }
    }

    QStringList etr = cacheFolder.entryList(QDir::NoDotAndDotDot);
    // if not 256 subdirs, we re-create the cache
    if (etr.size() < 256) {
        Q_FOREACH (const QString &e, etr) {
            cacheFolder.rmdir(e);
        }
    }
    if (!isTiled) {
        for (U32 i = 0x00; i <= 0xF; ++i) {
            for (U32 j = 0x00; j <= 0xF; ++j) {
                std::ostringstream oss;
                oss << std::hex << i;
                oss << std::hex << j;
                std::string str = oss.str();
                bool success = cacheFolder.mkdir( QString::fromUtf8( str.c_str() ) );
                if (!success) {
                    qDebug() << "Warning: cache directory" << (cachePath.toStdString() + '/' + str).c_str() << "could not be created";
                }
            }
        }
    }
}

void
AppManagerPrivate::setMaxCacheFiles()
{
    /*Default to something reasonable if the code below would happen to not work for some reason*/
    size_t hardMax = NATRON_MAX_CACHE_FILES_OPENED;

#if defined(Q_OS_UNIX) && defined(RLIMIT_NOFILE)
    /*
       Avoid 'Too many open files' on Unix.

       Increase the number of file descriptors that the process can open to the maximum allowed.
       - By default, Mac OS X only allows 256 file descriptors, which can easily be reached.
       - On Linux, the default limit is usually 1024.

        Note that due to a bug in stdio on OS X, the limit on the number of files opened using fopen()
        cannot be changed after the first call to stdio (e.g. printf() or fopen()).
        Consequently, this has to be the first thing to do in main().
     */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        if (rl.rlim_max > rl.rlim_cur) {
            rl.rlim_cur = rl.rlim_max;
            if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
#             if defined(__APPLE__) && defined(OPEN_MAX)
                // On Mac OS X, setrlimit(RLIMIT_NOFILE, &rl) fails to set
                // rlim_cur above OPEN_MAX even if rlim_max > OPEN_MAX.
                if (rl.rlim_cur > OPEN_MAX) {
                    rl.rlim_cur = OPEN_MAX;
                    hardMax = rl.rlim_cur;
                    setrlimit(RLIMIT_NOFILE, &rl);
                }
#             endif
            } else {
                hardMax = rl.rlim_cur;
            }
        }
    }
    //#elif defined(Q_OS_WIN)
    // The following code sets the limit for stdio-based calls only.
    // Note that low-level calls (CreateFile(), WriteFile(), ReadFile(), CloseHandle()...) are not affected by this limit.
    // References:
    // - http://msdn.microsoft.com/en-us/library/6e3b887c.aspx
    // - https://stackoverflow.com/questions/870173/is-there-a-limit-on-number-of-open-files-in-windows/4276338
    // - http://bugs.mysql.com/bug.php?id=24509
    //_setmaxstdio(2048); // sets the limit for stdio-based calls
    // On Windows there seems to be no limit at all. The following test program can prove it:
    /*
       #include <windows.h>
       int
       main(int argc,
         char *argv[])
       {
        const int maxFiles = 10000;

        std::list<HANDLE> files;

        for (int i = 0; i < maxFiles; ++i) {
            std::stringstream ss;
            ss << "C:/Users/Lex/Documents/GitHub/Natron/App/win32/debug/testMaxFiles/file" << i ;
            std::string filename = ss.str();
            HANDLE file_handle = ::CreateFile(filename.c_str(), GENERIC_READ | GENERIC_WRITE,
                                              0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);


            if (file_handle != INVALID_HANDLE_VALUE) {
                files.push_back(file_handle);
                std::cout << "Good files so far: " << files.size() << std::endl;

            } else {
                char* message ;
                FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL,GetLastError(),0,(LPSTR)&message,0,NULL);
                std::cout << "Failed to open " << filename << ": " << message << std::endl;
                LocalFree(message);
            }
        }
        std::cout << "Total opened files: " << files.size() << std::endl;
        for (std::list<HANDLE>::iterator it = files.begin(); it != files.end(); ++it) {
            CloseHandle(*it);
        }
       }
     */
#endif

    maxCacheFiles = hardMax * 0.9;
}

void
AppManagerPrivate::copyUtf8ArgsToMembers(const std::vector<std::string>& utf8Args)
{
    // Copy command line args to local members that live throughout the lifetime of AppManager
    commandLineArgsWideOriginal.resize(utf8Args.size());
    commandLineArgsUtf8Original.resize(utf8Args.size());
    nArgs = (int)utf8Args.size();
    for (std::size_t i = 0; i < utf8Args.size(); ++i) {
        commandLineArgsUtf8Original[i] = strdup(utf8Args[i].c_str());

        // Python 3 needs wchar_t arguments
        commandLineArgsWideOriginal[i] = char2wchar(utf8Args[i].c_str());
    }
    commandLineArgsUtf8 = commandLineArgsUtf8Original;
    commandLineArgsWide = commandLineArgsWideOriginal;
}

void
AppManagerPrivate::handleCommandLineArgs(int argc, char** argv)
{
    // Ensure the arguments are Utf-8 encoded
    std::vector<std::string> utf8Args;
    if (argv) {
        CLArgs::ensureCommandLineArgsUtf8(argc, argv, &utf8Args);
    } else {
        // If the user didn't specify launch arguments (e.g unit testing),
        // At least append the binary path
        std::string path = ProcInfo::applicationDirPath(0);
        utf8Args.push_back(path);
    }

    copyUtf8ArgsToMembers(utf8Args);
}

void
AppManagerPrivate::handleCommandLineArgsW(int argc, wchar_t** argv)
{
    std::vector<std::string> utf8Args;
    if (argv) {
        for (int i = 0; i < argc; ++i) {
            std::wstring wide(argv[i]);
            std::string str = StrUtils::utf16_to_utf8(wide);
            assert(StrUtils::is_utf8(str.c_str()));
            utf8Args.push_back(str);
        }
    } else {
        // If the user didn't specify launch arguments (e.g unit testing),
        // At least append the binary path
        std::string path = ProcInfo::applicationDirPath(0);
        utf8Args.push_back(path);
    }

    copyUtf8ArgsToMembers(utf8Args);
}

NATRON_NAMESPACE_EXIT
