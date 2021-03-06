
/******************************************************************************
 *
 *  This file is part of canu, a software program that assembles whole-genome
 *  sequencing reads into contigs.
 *
 *  This software is based on:
 *    'Celera Assembler' (http://wgs-assembler.sourceforge.net)
 *    the 'kmer package' (http://kmer.sourceforge.net)
 *  both originally distributed by Applera Corporation under the GNU General
 *  Public License, version 2.
 *
 *  Canu branched from Celera Assembler at its revision 4587.
 *  Canu branched from the kmer project at its revision 1994.
 *
 *  This file is derived from:
 *
 *    src/AS_OVS/AS_OVS_overlapFile.C
 *    src/AS_OVS/AS_OVS_overlapFile.c
 *
 *  Modifications by:
 *
 *    Brian P. Walenz from 2007-FEB-28 to 2013-SEP-22
 *      are Copyright 2007-2009,2011-2013 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Sergey Koren on 2011-MAR-31
 *      are Copyright 2011 Battelle National Biodefense Institute, and
 *      are subject to the BSD 3-Clause License
 *
 *    Gregory Sims on 2012-FEB-01
 *      are Copyright 2012 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Brian P. Walenz from 2014-DEC-09 to 2015-JUN-16
 *      are Copyright 2014-2015 Battelle National Biodefense Institute, and
 *      are subject to the BSD 3-Clause License
 *
 *    Brian P. Walenz beginning on 2016-JAN-11
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *  File 'README.licenses' in the root directory of this distribution contains
 *  full conditions and disclaimers for each license.
 */

#include "ovStore.H"
#include "snappy.h"

//  The histogram associated with this is written to files with any suffices stripped off.



char *
ovFile::createDataName(char       *name,
                       const char *storeName,
                       uint32      sliceNum,
                       uint32      pieceNum) {

  snprintf(name, FILENAME_MAX, "%s/%04u<%03u>", storeName, sliceNum, pieceNum);

  return(name);
}



ovFile::ovFile(sqStore     *seq,
               const char  *filename,
               ovFileType   type,
               uint32       bufferSize) {
  construct(seq, filename, type, bufferSize);
}



ovFile::ovFile(sqStore     *seq,
               const char  *ovlName,
               uint32       sliceNum,
               uint32       pieceNum,
               ovFileType   type,
               uint32       bufferSize) {
  char  filename[FILENAME_MAX+1];

  createDataName(filename, ovlName, sliceNum, pieceNum);

  construct(seq, filename, type, bufferSize);
}



ovFile::~ovFile() {

  writeBuffer(true);

  AS_UTL_closeFile(_file, _name);

  if ((_isOutput) && (_histogram))
    _histogram->saveHistogram(_prefix);

  if (_isTemporary)
    AS_UTL_unlink(_name);

  delete    _countsW;
  delete    _countsR;
  delete    _histogram;
  delete [] _buffer;
  delete [] _snappyBuffer;
}



void
ovFile::construct(sqStore     *seq,
                  const char  *name,
                  ovFileType   type,
                  uint32       bufferSize) {
  _seq       = seq;

  _countsW   = NULL;
  _countsR   = NULL;
  _histogram = NULL;

  //  We write two sizes of overlaps.  The 'normal' format doesn't contain the a_iid, while the
  //  'full' format does.  The buffer size must hold an integer number of overlaps, otherwise the
  //  reader will read partial overlaps and fail.  Choose a buffer size that can handle both.

  uint32  lcm = ((sizeof(uint32) * 1 + sizeof(ovOverlapDAT)) *
                 (sizeof(uint32) * 2 + sizeof(ovOverlapDAT)));

  if (bufferSize < 16 * 1024)
    bufferSize = 16 * 1024;

  _bufferLen    = 0;
  _bufferPos    = (bufferSize / (lcm * sizeof(uint32))) * lcm;  //  Forces reload on next read
  _bufferMax    = (bufferSize / (lcm * sizeof(uint32))) * lcm;
  _buffer       = new uint32 [_bufferMax];

  _snappyLen    = 0;
  _snappyBuffer = NULL;

  assert(_bufferMax % ((sizeof(uint32) * 1) + (sizeof(ovOverlapDAT))) == 0);
  assert(_bufferMax % ((sizeof(uint32) * 2) + (sizeof(ovOverlapDAT))) == 0);

  //  Create the input/output buffers and files.

  _isOutput    = false;
  _isNormal    = (type == ovFileNormal) || (type == ovFileNormalWrite);
  _useSnappy   = false;

  _isTemporary = false;

  memset(_prefix, 0, FILENAME_MAX+1);
  memset(_name,   0, FILENAME_MAX+1);

  strncpy(_name, name, FILENAME_MAX);
  AS_UTL_findBaseFileName(_prefix, _name);

  //
  //  Handle ovStore files.  These CANNOT be compressed, not even snappy.  We need
  //  random access to specific overlaps.
  //

  if (type == ovFileNormal)                         //  For store overlaps, fetch from
    _isTemporary = fetchFromObjectStore(_name);     //  the object store if needed.

  if (type == ovFileNormal) {
    _file        = AS_UTL_openInputFile(_name);
    _isOutput    = false;
    _useSnappy   = false;
    _histogram   = new ovStoreHistogram(_prefix);
  }

  if (type == ovFileNormalWrite) {
    _file        = AS_UTL_openOutputFile(_name);
    _isOutput    = true;
    _useSnappy   = false;
    _histogram   = new ovStoreHistogram(_seq);
    _countsW     = new ovFileOCW(_seq, NULL);
  }

  //
  //  Handle overlapper output files.  These can be compressed, but not really useful with
  //  snappy enabled.
  //
  //  ovFileFileWriteNoCounts is used for intermediate bucket files when constructing
  //  the store.  They CAN be compressed.
  //

  if (type == ovFileFull) {                       //  No automagic object store fetch;
    _file        = AS_UTL_openInputFile(_name);   //  the executive must do this for us.
    _isOutput    = false;
    _useSnappy   = true;
    _countsR     = new ovFileOCR(_seq, _prefix);
  }

  if (type == ovFileFullCounts) {
    _file        = NULL;
    _isOutput    = false;
    _useSnappy   = true;
    _countsR     = new ovFileOCR(_seq, _prefix);
  }

  if (type == ovFileFullWrite) {
    _file        = AS_UTL_openOutputFile(_name);
    _isOutput    = true;
    _useSnappy   = true;
    _countsW     = new ovFileOCW(_seq, _prefix);
  }

  //
  //  Handle store construction intermediate files.  These are full overlaps, but we
  //  don't need to save any histogram/count data.  They CAN be compressed.
  //

  if (type == ovFileFullWriteNoCounts) {
    _file        = AS_UTL_openOutputFile(_name);
    _isOutput    = true;
    _useSnappy   = true;
  }
}



void
ovFile::writeBuffer(bool force) {

  if (_isOutput == false)  //  Needed because it's called in the destructor.
    return;

  if ((force == false) && (_bufferLen < _bufferMax))
    return;
  if (_bufferLen == 0)
    return;

  //  If compressing, compress the block then write compressed length and the block.

  if (_useSnappy == true) {
    size_t   bl = snappy::MaxCompressedLength(_bufferLen * sizeof(uint32));

    if (_snappyLen < bl) {
      delete [] _snappyBuffer;
      _snappyLen    = bl;
      _snappyBuffer = new char [_snappyLen];
    }

    snappy::RawCompress((const char *)_buffer, _bufferLen * sizeof(uint32), _snappyBuffer, &bl);

    uint64 bl64 = bl;

    writeToFile(bl64,          "ovFile::writeBuffer::bl",     _file);  //  Snappy wants to use size_t, we want to use uint64 in files.
    writeToFile(_snappyBuffer, "ovFile::writeBuffer::sb", bl, _file);  //  MacOS claims size_t != uint64.
  }

  //  Otherwise, just dump the block

  else
    writeToFile(_buffer, "ovFile::writeBuffer", _bufferLen, _file);

  //  Buffer written.  Clear it.
  _bufferLen = 0;
}



void
ovFile::writeOverlap(ovOverlap *overlap) {

  assert(_isOutput == true);

  writeBuffer();

  if (_countsW)                      //  _countsW doesn't exist if we're writing
    _countsW->addOverlap(overlap);   //  store intermediate buckets.

  if (_histogram)
    _histogram->addOverlap(overlap);

  if (_isNormal == false)
    _buffer[_bufferLen++] = overlap->a_iid;

  _buffer[_bufferLen++] = overlap->b_iid;

#if (ovOverlapWORDSZ == 32)
  for (uint32 ii=0; ii<ovOverlapNWORDS; ii++)
    _buffer[_bufferLen++] = overlap->dat.dat[ii];
#endif

#if (ovOverlapWORDSZ == 64)
  for (uint32 ii=0; ii<ovOverlapNWORDS; ii++) {
    _buffer[_bufferLen++] = (overlap->dat.dat[ii] >> 32) & 0xffffffff;
    _buffer[_bufferLen++] = (overlap->dat.dat[ii])       & 0xffffffff;
  }
#endif

  assert(_bufferLen <= _bufferMax);
}



void
ovFile::writeOverlaps(ovOverlap *overlaps, uint64 overlapsLen) {

  assert(_isOutput == true);

  //  Add all overlaps to the buffer.

  for (uint32 oo=0; oo<overlapsLen; oo++) {
    writeBuffer();

    assert(_countsW != NULL);
    if (_countsW)
      _countsW->addOverlap(overlaps + oo);

    if (_histogram)
      _histogram->addOverlap(overlaps + oo);

    if (_isNormal == false)
      _buffer[_bufferLen++] = overlaps[oo].a_iid;

    _buffer[_bufferLen++] = overlaps[oo].b_iid;

#if (ovOverlapWORDSZ == 32)
    for (uint32 ii=0; ii<ovOverlapNWORDS; ii++)
      _buffer[_bufferLen++] = overlaps[oo].dat.dat[ii];
#endif

#if (ovOverlapWORDSZ == 64)
    for (uint32 ii=0; ii<ovOverlapNWORDS; ii++) {
      _buffer[_bufferLen++] = (overlaps[oo].dat.dat[ii] >> 32) & 0xffffffff;
      _buffer[_bufferLen++] = (overlaps[oo].dat.dat[ii])       & 0xffffffff;
    }
#endif
  }

  assert(_bufferLen <= _bufferMax);
}



void
ovFile::readBuffer(void) {

  if (_bufferPos < _bufferLen)
    return;

  //  Need to load a new buffer.  Everyone resets bufferPos to the start.

  _bufferPos = 0;

  //  If an uncompressed file, load as much as possible and return.  This is
  //  allowed and expected to have a short read at the end of the file.

  if (_useSnappy == false) {
    _bufferLen = loadFromFile(_buffer, "ovFile::readBuffer", _bufferMax, _file, false);
    return;
  }

  //  Otherwise, the data is compressed with snappy.
  //  First, read the length of the snappy buffer (allowing it to return if EOF is encountered),
  //  then, load the buffer and uncompress it (failing if the read is shorter than it should have been).

  uint64  cl64 = 0;                                                             //  MacOS is claiming size_t is different than uint64,
  uint64  clc  = loadFromFile(cl64, "ovFile::readBuffer::cl", _file, false);    //  but I want to use uint64 here for portability.

  resizeArray(_snappyBuffer, 0, _snappyLen, cl64, resizeArray_doNothing);

  uint64  sbc = loadFromFile(_snappyBuffer, "ovFile::readBuffer::sb", cl64, _file, false);

  if (sbc != cl64)
    fprintf(stderr, "ERROR: short read on file '%s': read " F_U64 " bytes, expected " F_U64 ".\n",
            _prefix, sbc, cl64), exit(1);

  size_t  ol = 0;

  snappy::GetUncompressedLength(_snappyBuffer, cl64, &ol);

  _bufferLen = ol / sizeof(uint32);

  assert(_bufferLen <= _bufferMax);

  snappy::RawUncompress(_snappyBuffer, cl64, (char *)_buffer);
}



bool
ovFile::readOverlap(ovOverlap *overlap) {

  assert(_isOutput == false);

  readBuffer();

  if (_bufferLen == 0)
    return(false);

  assert(_bufferPos < _bufferLen);

  if (_isNormal == false)
    overlap->a_iid      = _buffer[_bufferPos++];

  overlap->b_iid      = _buffer[_bufferPos++];

#if (ovOverlapWORDSZ == 32)
  for (uint32 ii=0; ii<ovOverlapNWORDS; ii++)
    overlap->dat.dat[ii] = _buffer[_bufferPos++];
#endif

#if (ovOverlapWORDSZ == 64)
  for (uint32 ii=0; ii<ovOverlapNWORDS; ii++) {
    overlap->dat.dat[ii]   = _buffer[_bufferPos++];
    overlap->dat.dat[ii] <<= 32;
    overlap->dat.dat[ii]  |= _buffer[_bufferPos++];
  }
#endif

  assert(_bufferPos <= _bufferLen);

  return(true);
}



uint64
ovFile::readOverlaps(ovOverlap *overlaps, uint64 overlapsLen) {
  uint64  nLoaded = 0;

  assert(_isOutput == false);

  while (nLoaded < overlapsLen) {
    readBuffer();

    if (_bufferLen == 0)
      return(nLoaded);

    assert(_bufferPos < _bufferLen);

    if (_isNormal == false)
      overlaps[nLoaded].a_iid      = _buffer[_bufferPos++];

    overlaps[nLoaded].b_iid      = _buffer[_bufferPos++];

#if (ovOverlapWORDSZ == 32)
  for (uint32 ii=0; ii<ovOverlapNWORDS; ii++)
    overlaps[nLoaded].dat.dat[ii] = _buffer[_bufferPos++];
#endif

#if (ovOverlapWORDSZ == 64)
  for (uint32 ii=0; ii<ovOverlapNWORDS; ii++) {
    overlaps[nLoaded].dat.dat[ii]   = _buffer[_bufferPos++];
    overlaps[nLoaded].dat.dat[ii] <<= 32;
    overlaps[nLoaded].dat.dat[ii]  |= _buffer[_bufferPos++];
  }
#endif

    nLoaded++;

    assert(_bufferPos <= _bufferLen);
  }

  return(nLoaded);
}



//  Move to the correct spot, and force a load on the next readOverlap by setting the position to
//  the end of the buffer.
void
ovFile::seekOverlap(off_t overlap) {

  AS_UTL_fseek(_file, overlap * recordSize(), SEEK_SET);

  _bufferPos = _bufferLen;  //  We probably need to reload the buffer.
}




//  Well, shoot.  We can't know ovStoreHistogram in
//  ovStoreFile.H, so we can't delete it there.
void
ovFile::removeHistogram(void) {
  delete _histogram;
  _histogram = NULL;
}
