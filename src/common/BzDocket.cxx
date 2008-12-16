
#include "common.h"

// interface
#include "BzDocket.h"

// system
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <iostream>
using std::string;
using std::vector;
using std::map;

// common
#include "bzfio.h"
#include "Pack.h"
#include "OSFile.h"
#include "FileManager.h"


const char* BzDocket::magic = "BzDocket";

string BzDocket::errorMsg = "";

#ifndef _WIN32
  static const char dirSep = '/';
#else
  static const char dirSep = '\\';
#endif

/******************************************************************************/

BzDocket::BzDocket()
{
}


BzDocket::BzDocket(const string& name) : docketName(name)
{
}

BzDocket::~BzDocket()
{
}


/******************************************************************************/

size_t BzDocket::packSize() const
{
  size_t fullSize = 0;
  fullSize += strlen(magic);
  fullSize += sizeof(uint32_t); // version
  fullSize += nboStdStringPackSize(docketName);
  fullSize += sizeof(uint32_t); // file count
  FileMap::const_iterator it;
  for (it = fileMap.begin(); it != fileMap.end(); ++ it) {
    fullSize += nboStdStringPackSize(it->first);
    fullSize += sizeof(uint32_t); // offset
    fullSize += sizeof(uint32_t); // extra
  }
  for (it = fileMap.begin(); it != fileMap.end(); ++ it) {
    fullSize += nboStdStringPackSize(it->second);
  }
  return fullSize;
}


void* BzDocket::pack(void* buf) const
{
  buf = nboPackString(buf, magic, strlen(magic));
  buf = nboPackUInt(buf, 0);              // version
  buf = nboPackStdString(buf, docketName);
  buf = nboPackUInt(buf, fileMap.size()); // file count
  FileMap::const_iterator it;
  uint32_t offset = 0; 
  for (it = fileMap.begin(); it != fileMap.end(); ++ it) {
    logDebugMessage(3, "packing into %s: (%i) '%s'\n", docketName.c_str(), 
                    (int)it->second.size(), it->first.c_str());
    buf = nboPackStdString(buf, it->first);
    buf = nboPackUInt(buf, offset);
    buf = nboPackUInt(buf, 0); // extra
    offset += it->second.size();
  }
  for (it = fileMap.begin(); it != fileMap.end(); ++ it) {
    buf = nboPackStdString(buf, it->second);
  }
  return buf;
}


void* BzDocket::unpack(void* buf)
{
  char tmp[256];
  buf = nboUnpackString(buf, tmp, strlen(magic));
  if (strncmp(magic, tmp, strlen(magic)) != 0) {
    errorMsg = "bad magic";
    return NULL;
  }
  uint32_t version;
  buf = nboUnpackUInt(buf, version); // version
  if (version != 0) {
    errorMsg = "bad version";
    return NULL;
  }

  buf = nboUnpackStdString(buf, docketName);

  uint32_t count;
  buf = nboUnpackUInt(buf, count);
  vector<string> names;
  for (uint32_t i = 0; i < count; i++) {
    string name;
    buf = nboUnpackStdString(buf, name);
    names.push_back(name);

    uint32_t offset, extra;
    buf = nboUnpackUInt(buf, offset);
    buf = nboUnpackUInt(buf, extra);
  }

  for (uint32_t i = 0; i < count; i++) {
    string data;
    buf = nboUnpackStdString(buf, data);
    fileMap[names[i]] = data;
    logDebugMessage(3, "unpacked from %s: (%i) '%s'\n", docketName.c_str(),
                    (int)data.size(), names[i].c_str());
  }
  return buf;
}



/******************************************************************************/

static string getMapPath(const std::string& path)
{
  string p = path;
  std::replace(p.begin(), p.end(), '\\', '/');  
  return p;
}


bool BzDocket::addData(const std::string& data, const std::string& mapPath)
{
  if (mapPath.find('\\') != string::npos) {
    errorMsg = "bad backslash";
    printf("internal BzDocket error: %s\n", errorMsg.c_str());
    return false;
  }

  errorMsg = "";
  if (fileMap.find(mapPath) != fileMap.end()) {
    errorMsg = "duplicate";
    return false;
  }

  fileMap[mapPath] = data;

  return true;
}


bool BzDocket::addDir(const std::string& dirPath, const std::string& mapPrefix)
{
  errorMsg = "";

  if (dirPath.empty()) {
    errorMsg = "blank directory name";
    return false;
  }

  string realDir = dirPath;
  if (realDir[realDir.size() - 1] != dirSep) {
    realDir += dirSep;
  }

  OSDir dir(realDir);
  const size_t dirLen = dir.getStdName().size();

  OSFile file;
  while (dir.getNextFile(file, true)) { // recursive
    const string truncPath = file.getStdName().substr(dirLen);
    addFile(file.getStdName(), getMapPath(mapPrefix + truncPath));
  }

  return true;
}


bool BzDocket::addFile(const std::string& filePath, const std::string& mapPath)
{
  errorMsg = "";
  if (fileMap.find(mapPath) != fileMap.end()) {
    errorMsg = "duplicate";
    return false;
  }

  FILE* file = fopen(filePath.c_str(), "r");
  if (file == NULL) {
    errorMsg = strerror(errno);
    return false;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    errorMsg = strerror(errno);
    fclose(file);
    return false;
  }

  const long len = ftell(file);
  if (len == -1) {
    errorMsg = strerror(errno);
    fclose(file);
    return false;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    errorMsg = strerror(errno);
    fclose(file);
    return false;
  }

  char* buf = new char[len];
  if (fread(buf, 1, len, file) != (size_t)len) {
    errorMsg = strerror(errno);
    fclose(file);
    delete[] buf;
    return false;
  }

  fclose(file);

  const string data(buf, len);
  delete[] buf;

  logDebugMessage(3, "adding to %s: (%li) '%s' as '%s'\n", docketName.c_str(),
                  len, filePath.c_str(), mapPath.c_str());

  return addData(data, mapPath);
}


/******************************************************************************/

bool BzDocket::findFile(const std::string& mapPath, std::string& data)
{
  FileMap::const_iterator it = fileMap.find(mapPath);
  if (it == fileMap.end()) {
    return false;
  }
  data = it->second;  
  return true;
}


/******************************************************************************/

static bool createParentDirs(const string& path)
{
  string::size_type pos = 0;
  for (pos = path.find('/');
       pos != string::npos;
       pos = path.find('/', pos + 1)) {
    OSDir dir;
    dir.makeOSDir(path.substr(0, pos));
  }
  return true;
}


bool BzDocket::save(const std::string& dirPath)
{
  if (dirPath.empty()) {
    return false;
  }

  string realDir = dirPath;
  if (realDir[realDir.size() - 1] != dirSep) {
    realDir += dirSep;
  }

  FileMap::const_iterator it;
  for (it = fileMap.begin(); it != fileMap.end(); ++it) {
    const string fullPath = realDir + it->first;
    if (!createParentDirs(fullPath)) {
      continue;
    }
    FILE* file = fopen(fullPath.c_str(), "wb");
    if (file == NULL) {
      continue;
    }
    const string& data = it->second;
    fwrite(data.data(), 1, data.size(), file);
    fclose(file);
  }

  return true;
}


/******************************************************************************/
