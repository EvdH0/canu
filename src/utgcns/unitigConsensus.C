
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
 *    src/AS_CNS/MultiAlignUnitig.C
 *    src/AS_CNS/MultiAlignUnitig.c
 *    src/AS_CNS/MultiAlignment_CNS.c
 *    src/utgcns/libcns/MultiAlignUnitig.C
 *    src/utgcns/libcns/unitigConsensus.C
 *
 *  Modifications by:
 *
 *    Michael Schatz on 2004-SEP-23
 *      are Copyright 2004 The Institute for Genomics Research, and
 *      are subject to the GNU General Public License version 2
 *
 *    Jason Miller on 2005-MAR-22
 *      are Copyright 2005 The Institute for Genomics Research, and
 *      are subject to the GNU General Public License version 2
 *
 *    Eli Venter from 2005-MAR-30 to 2008-FEB-13
 *      are Copyright 2005-2006,2008 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Gennady Denisov from 2005-MAY-09 to 2008-JUN-06
 *      are Copyright 2005-2008 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Brian P. Walenz from 2005-JUN-16 to 2013-OCT-04
 *      are Copyright 2005-2013 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Aaron Halpern from 2005-SEP-29 to 2006-OCT-03
 *      are Copyright 2005-2006 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Sergey Koren from 2008-FEB-27 to 2009-JUN-05
 *      are Copyright 2008-2009 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Sergey Koren on 2011-OCT-27
 *      are Copyright 2011 Battelle National Biodefense Institute, and
 *      are subject to the BSD 3-Clause License
 *
 *    Brian P. Walenz from 2014-NOV-17 to 2015-AUG-11
 *      are Copyright 2014-2015 Battelle National Biodefense Institute, and
 *      are subject to the BSD 3-Clause License
 *
 *    Brian P. Walenz beginning on 2015-OCT-09
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *    Sergey Koren beginning on 2015-DEC-17
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *  File 'README.licenses' in the root directory of this distribution contains
 *  full conditions and disclaimers for each license.
 */

#include "unitigConsensus.H"

// for pbdagcon
#include "Alignment.H"
#include "AlnGraphBoost.H"
#include "edlib.H"

#include <set>

using namespace std;


abSequence::abSequence(uint32  readID,
                       uint32  length,
                       char   *seq,
                       uint8  *qlt,
                       uint32  complemented) {
  _iid              = readID;
  _length           = length;
  _complement       = complemented;

  _bases            = new char  [_length + 1];
  _quals            = new uint8 [_length + 1];

  //  Make a complement table

  char inv[256] = {0};

  inv['a'] = 't';  inv['A'] = 'T';
  inv['c'] = 'g';  inv['C'] = 'G';
  inv['g'] = 'c';  inv['G'] = 'C';
  inv['t'] = 'a';  inv['T'] = 'A';
  inv['n'] = 'n';  inv['N'] = 'N';
  inv['-'] = '-';

  //  Stash the bases/quals

  for (uint32 ii=0; ii<_length; ii++)
    assert((seq[ii] == 'A') ||
           (seq[ii] == 'C') ||
           (seq[ii] == 'G') ||
           (seq[ii] == 'T') ||
           (seq[ii] == 'N'));

  if (complemented == false)
    for (uint32 ii=0, pp=0; ii<_length; ii++, pp++) {
      _bases[pp] = seq[ii];
      _quals[pp] = qlt[ii];
    }

  else
    for (uint32 ii=_length, pp=0; ii-->0; pp++) {
      _bases[pp] = inv[ seq[ii] ];
      _quals[pp] =      qlt[ii];
    }

  _bases[_length] = 0;  //  NUL terminate the strings so we can use them in aligners.
  _quals[_length] = 0;  //  Not actually a string, the 0 is just another QV=0 entry.
};



unitigConsensus::unitigConsensus(sqStore  *seqStore_,
                                 double    errorRate_,
                                 double    errorRateMax_,
                                 uint32    minOverlap_) {

  _seqStore        = seqStore_;

  _tig             = NULL;
  _numReads        = 0;

  _sequencesMax   = 0;
  _sequencesLen   = 0;
  _sequences      = NULL;

  _utgpos          = NULL;
  _cnspos          = NULL;

  _minOverlap      = minOverlap_;
  _errorRate       = errorRate_;
  _errorRateMax    = errorRateMax_;
}


unitigConsensus::~unitigConsensus() {
  delete [] _utgpos;
  delete [] _cnspos;
}



void
unitigConsensus::addRead(uint32   readID,
                         uint32   askip, uint32 bskip,
                         bool     complemented,
                         map<uint32, sqRead *>     *inPackageRead,
                         map<uint32, sqReadData *> *inPackageReadData) {

  //  Grab the read.  If there is no package, load the read from the store.  Otherwise, load the
  //  read from the package.  This REQUIRES that the package be in-sync with the unitig.  We fail
  //  otherwise.  Hey, it's used for debugging only...

  sqRead      *read     = NULL;
  sqReadData  *readData = NULL;

  if (inPackageRead == NULL) {
    read     = _seqStore->sqStore_getRead(readID);
    readData = new sqReadData;

    _seqStore->sqStore_loadReadData(read, readData);
  }

  else {
    read     = (*inPackageRead)[readID];
    readData = (*inPackageReadData)[readID];
  }

  assert(read     != NULL);
  assert(readData != NULL);

  //  Grab seq/qlt from the read, offset to the proper begin and length.

  uint32  seqLen = read->sqRead_sequenceLength() - askip - bskip;
  char   *seq    = readData->sqReadData_getSequence()  + ((complemented == false) ? askip : bskip);
  uint8  *qlt    = readData->sqReadData_getQualities() + ((complemented == false) ? askip : bskip);

  //  Add it to our list.

  increaseArray(_sequences, _sequencesLen, _sequencesMax, 1);

  _sequences[_sequencesLen++] = new abSequence(readID, seqLen, seq, qlt, complemented);

  delete readData;
}



bool
unitigConsensus::initialize(map<uint32, sqRead *>     *reads,
                            map<uint32, sqReadData *> *datas) {

  if (_numReads == 0) {
    fprintf(stderr, "utgCns::initialize()-- unitig has no children.\n");
    return(false);
  }

  _utgpos = new tgPosition [_numReads];
  _cnspos = new tgPosition [_numReads];

  memcpy(_utgpos, _tig->getChild(0), sizeof(tgPosition) * _numReads);
  memcpy(_cnspos, _tig->getChild(0), sizeof(tgPosition) * _numReads);

  //  Clear the cnspos position.  We use this to show it's been placed by consensus.
  //  Guess the number of columns we'll end up with.
  //  Initialize abacus with the reads.

  for (int32 i=0; i<_numReads; i++) {
    _cnspos[i].setMinMax(0, 0);

    addRead(_utgpos[i].ident(),
            _utgpos[i]._askip, _utgpos[i]._bskip,
            _utgpos[i].isReverse(),
            reads,
            datas);
  }

  //  Check for duplicate reads

  {
    set<uint32>  dupFrag;

    for (uint32 i=0; i<_numReads; i++) {
      if (_utgpos[i].isRead() == false) {
        fprintf(stderr, "unitigConsensus()-- Unitig %d FAILED.  Child %d is not a read.\n",
                _tig->tigID(), _utgpos[i].ident());
        return(false);
      }

      if (dupFrag.find(_utgpos[i].ident()) != dupFrag.end()) {
        fprintf(stderr, "unitigConsensus()-- Unitig %d FAILED.  Child %d is a duplicate.\n",
                _tig->tigID(), _utgpos[i].ident());
        return(false);
      }

      dupFrag.insert(_utgpos[i].ident());
    }
  }

  return(true);
}




char *
unitigConsensus::generateTemplateStitch(void) {
  int32   minOlap  = 500;

  //  Initialize, copy the first read.

  uint32       rid      = 0;

  abSequence  *seq      = getSequence(rid);
  char        *fragment = seq->getBases();
  uint32       readLen  = seq->length();

  uint32       tigmax = AS_MAX_READLEN;  //  Must be at least AS_MAX_READLEN, else resizeArray() could fail
  uint32       tiglen = 0;
  char        *tigseq = NULL;

  allocateArray(tigseq, tigmax, resizeArray_clearNew);

  if (showAlgorithm()) {
    fprintf(stderr, "\n");
    fprintf(stderr, "generateTemplateStitch()-- COPY READ read #%d %d (len=%d to %d-%d)\n",
            0, _utgpos[0].ident(), readLen, _utgpos[0].min(), _utgpos[0].max());
  }

  for (uint32 ii=0; ii<readLen; ii++)
    tigseq[tiglen++] = fragment[ii];

  tigseq[tiglen] = 0;

  uint32       ePos = _utgpos[0].max();   //  Expected end of template, from bogart supplied positions.


  //  Find the next read that has some minimum overlap and a large extension, copy that into the template.

  //  Align read to template.  Expecting to find alignment:
  //
  //        template ---------------
  //        read             ---------------
  //                               ^
  //
  //  All we need to find is where the template ends on the read, the ^ above.  We know the
  //  expected size of the overlap, and can extract those bases from the template and look for a
  //  full alignment to the read.
  //
  //  We'll align 80% of the expected overlap to the read, allowing a 20% buffer on either end.
  //
  //                        |  +-80% expected overlap size
  //                        |  |     +-fPos
  //                        v  v     v
  //        template ----------(-----)
  //        read            (-----------)------
  //

  while (rid < _numReads) {
    uint32 nr = 0;  //  Next read
    uint32 nm = 0;  //  Next read maximum position

    //  Pick the next read as the one with the longest extension from all with some minimum overlap
    //  to the template

    if (showAlgorithm())
      fprintf(stderr, "\n");

    for (uint32 ii=rid+1; ii < _numReads; ii++) {

      //  If contained, move to the next read.  (Not terribly useful to log, so we don't)

      if (_utgpos[ii].max() < ePos)
        continue;

      //  If a bigger end position, save the overlap.  One quirk: if we've already saved an overlap, and this
      //  overlap is thin, don't save the thin overlap.

      bool   thick = (_utgpos[ii].min() + minOlap < ePos);
      bool   first = (nm == 0);
      bool   save  = false;

      if ((nm < _utgpos[ii].max()) && (thick || first)) {
        save = true;
        nr   = ii;
        nm   = _utgpos[ii].max();
      }

      if (showAlgorithm())
        fprintf(stderr, "generateTemplateStitch()-- read #%d/%d ident %d position %d-%d%s%s%s\n",
                ii, _numReads, _utgpos[ii].ident(), _utgpos[ii].min(), _utgpos[ii].max(),
                (save  == true)  ? " SAVE"  : "",
                (thick == false) ? " THIN"  : "",
                (first == true)  ? " FIRST" : "");


      //  If this read has an overlap smaller than we want, stop searching.

      if (thick == false)
        break;
    }

    if (nr == 0) {
      if (showAlgorithm())
        fprintf(stderr, "generateTemplateStitch()-- NO MORE READS TO ALIGN\n");
      break;
    }

    assert(nr != 0);

    rid      = nr;        //  We'll place read 'nr' in the template.

    seq      = getSequence(rid);
    fragment = seq->getBases();
    readLen  = seq->length();

    int32  readBgn;
    int32  readEnd;

    EdlibAlignResult result;
    bool             aligned       = false;

    double           templateSize  = 0.80;
    double           extensionSize = 0.20;

    int32            olapLen      = ePos - _utgpos[nr].min();  //  The expected size of the overlap
    int32            templateLen  = 0;
    int32            extensionLen = 0;

  alignAgain:
    templateLen  = (int32)ceil(olapLen * templateSize);    //  Extract 80% of the expected overlap size
    extensionLen = (int32)ceil(olapLen * extensionSize);   //  Extend read by 20% of the expected overlap size

    readBgn = 0;
    readEnd = olapLen + extensionLen;

    if (readEnd > readLen)
      readEnd = readLen;
    // enforce minimum template length
    if (templateLen <= 1)
       templateLen ++;

    if (showAlgorithm()) {
      fprintf(stderr, "\n");
      fprintf(stderr, "generateTemplateStitch()-- ALIGN template %d-%d (len=%d) to read #%d %d %d-%d (len=%d actual=%d at %d-%d)  expecting olap of %d\n",
              tiglen - templateLen, tiglen, templateLen,
              nr, _utgpos[nr].ident(), readBgn, readEnd, readEnd - readBgn, readLen,
              _utgpos[nr].min(), _utgpos[nr].max(),
              olapLen);
    }

    result = edlibAlign(tigseq + tiglen - templateLen, templateLen,
                        fragment, readEnd - readBgn,
                        edlibNewAlignConfig(olapLen * _errorRate, EDLIB_MODE_HW, EDLIB_TASK_PATH));

    //  We're expecting the template to align inside the read.
    //
    //                                                        v- always the end
    //    TEMPLATE  --------------------------[---------------]
    //    READ                          [------------------------------]---------
    //                always the start -^
    //
    //  If we don't find an alignment at all, we move the template start point to the right (making
    //  the template smaller) and also move the read end point to the right (making the read
    //  bigger).

    bool   tryAgain = false;

    bool   noResult      = (result.numLocations == 0);
    bool   gotResult     = (result.numLocations  > 0);

    bool   hitTheStart   = (gotResult) && (result.startLocations[0] == 0);

    bool   hitTheEnd     = (gotResult) && (result.endLocations[0] + 1 == readEnd - readBgn);
    bool   moreToExtend  = (readEnd < readLen);

    //  Reset if the edit distance is waay more than our error rate allows.  This seems to be a quirk with
    //  edlib when aligning to N's - I got startLocation = endLocation = 0 and editDistance = alignmentLength.

    if ((double)result.editDistance / result.alignmentLength > _errorRate) {
      noResult    = true;
      gotResult   = false;
      hitTheStart = false;
      hitTheEnd   = false;
    }

    //  HOWEVER, if we get a result and it's near perfect, declare success even if we hit the start.
    //  These are simple repeats that will align with any overlap.  The one BPW debugged was 99+% A.

    if ((gotResult == true) &&
        (hitTheStart == true) &&
        ((double)result.editDistance / result.alignmentLength < 0.1)) {
      hitTheStart = false;
    }

    //  NOTE that if we hit the end with the same conditions, we should try again, unless there
    //  isn't anything left.  In that case, we don't extend the template.

    if ((gotResult == true) &&
        (hitTheEnd == true) &&
        (moreToExtend == false) &&
        ((double)result.editDistance / result.alignmentLength < 0.1)) {
      hitTheEnd = false;
    }

    //  Now, report what happened, and maybe try again.

    if ((showAlgorithm()) && (noResult == true))
      fprintf(stderr, "generateTemplateStitch()-- FAILED to align - no result\n");

    if ((showAlgorithm()) && (noResult == false))
      fprintf(stderr, "generateTemplateStitch()-- FOUND alignment at %d-%d editDist %d alignLen %d %.f%%\n",
              result.startLocations[0], result.endLocations[0]+1,
              result.editDistance,
              result.alignmentLength,
              100.0 * result.editDistance / result.alignmentLength);

    if ((noResult) || (hitTheStart)) {
      if (showAlgorithm())
        fprintf(stderr, "generateTemplateStitch()-- FAILED to align - %s - decrease template size by 10%%\n",
                (noResult == true) ? "no result" : "hit the start");
      tryAgain = true;
      templateSize -= 0.10;
    }

    if ((noResult) || (hitTheEnd && moreToExtend)) {
      if (showAlgorithm())
        fprintf(stderr, "generateTemplateStitch()-- FAILED to align - %s - increase read size by 10%%\n",
                (noResult == true) ? "no result" : "hit the end");
      tryAgain = true;
      extensionSize += 0.10;
    }

    if (templateSize < 0.01) {
      fprintf(stderr, "generateTemplateStitch()-- FAILED to align - no more template to remove!  Fail!\n");
      tryAgain = false;
    }

    if (tryAgain && noResult) {
      edlibFreeAlignResult(result);
      goto alignAgain;
    }

    //  Use the alignment (or the overlap) to figure out what bases in the read
    //  need to be appended to the template.

    if (noResult == false) {
      readBgn = result.startLocations[0];     //  Expected to be zero
      readEnd = result.endLocations[0] + 1;   //  Where we need to start copying the read

      if (showAlgorithm())
        fprintf(stderr, "generateTemplateStitch()-- Aligned template %d-%d to read %u %d-%d; copy read %d-%d to template.\n",
                tiglen - templateLen, tiglen, nr, readBgn, readEnd, readEnd, readLen);
    } else {
      readBgn = 0;
      readEnd = olapLen;

      if (showAlgorithm())
        fprintf(stderr, "generateTemplateStitch()-- Alignment failed, use original overlap; copy read %d-%d to template.\n",
                readEnd, readLen);
    }

    edlibFreeAlignResult(result);


    resizeArray(tigseq, tiglen, tigmax, tiglen + readLen - readEnd + 1);

    //  Append the read bases to the template.  If the alignment failed, reset any template bases
    //  that are N to be the read base.

    for (uint32 ii=readEnd; ii<readLen; ii++)
      tigseq[tiglen++] = fragment[ii];

    if (noResult == true)
      for (uint32 ii=0, jj=tiglen-readLen; ii<readLen; ii++)
        if (tigseq[jj] == 'N')
          tigseq[jj] = fragment[ii];

    tigseq[tiglen] = 0;

    assert(tiglen < tigmax);

    ePos = _utgpos[rid].max();

    if (showAlgorithm())
      fprintf(stderr, "generateTemplateStitch()-- Template now length %d, expected %d, difference %7.4f%%\n",
              tiglen, ePos, 200.0 * ((int32)tiglen - (int32)ePos) / ((int32)tiglen + (int32)ePos));
  }

  //  Report the expected and final size.  Guard against long tigs getting chopped.

  double  pd = 200.0 * ((int32)tiglen - (int32)ePos) / ((int32)tiglen + (int32)ePos);

  if (showAlgorithm()) {
    fprintf(stderr, "\n");
    fprintf(stderr, "generateTemplateStitch()-- generated template of length %d, expected length %d, %7.4f%% difference.\n",
            tiglen, ePos, pd);
  }

  if ((tiglen >= 100000) && ((pd < -50.0) || (pd > 50.0)))
    fprintf(stderr, "generateTemplateStitch()-- significant size difference, stopping.\n");
  assert((tiglen < 100000) || ((-50.0 <= pd) && (pd <= 50.0)));

  return(tigseq);
}



bool
alignEdLib(dagAlignment      &aln,
           tgPosition        &utgpos,
           char              *fragment,
           uint32             fragmentLength,
           char              *tigseq,
           uint32             tiglen,
           double             lengthScale,
           double             errorRate,
           bool               verbose) {

  EdlibAlignResult align;

  int32   padding        = (int32)ceil(fragmentLength * 0.10);
  double  bandErrRate    = errorRate / 2;
  bool    aligned        = false;
  double  alignedErrRate = 0.0;

  //  Decide on where to align this read.

  //  But, the utgpos positions are largely bogus, especially at the end of the tig.  utgcns (the
  //  original) used to track positions of previously placed reads, find an overlap beterrn this
  //  read and the last read, and use that info to find the coordinates for the new read.  That was
  //  very complicated.  Here, we just linearly scale.

  int32  tigbgn = max((int32)0,      (int32)floor(lengthScale * utgpos.min() - padding));
  int32  tigend = min((int32)tiglen, (int32)floor(lengthScale * utgpos.max() + padding));

  if (verbose)
    fprintf(stderr, "alignEdLib()-- align read %7u eRate %.4f at %9d-%-9d", utgpos.ident(), bandErrRate, tigbgn, tigend);

  //  This occurs if we don't lengthScale the positions.

  if (tigend < tigbgn) {
    fprintf(stderr, "alignEdLib()-- WARNING: tigbgn %d > tigend %d - tiglen %d utgpos %d-%d padding %d\n",
            tigbgn, tigend, tiglen, utgpos.min(), utgpos.max(), padding);
    // try to align it to full
    tigbgn = 0;
    tigend = utgpos.max();
    fprintf(stderr, "alignEdLib()-- WARNING: updated tigbgn %d > tigend %d - tiglen %d utgpos %d-%d padding %d\n",
            tigbgn, tigend, tiglen, utgpos.min(), utgpos.max(), padding);
  }
  assert(tigend > tigbgn);

  //  Align!  If there is an alignment, compute error rate and declare success if acceptable.

  align = edlibAlign(fragment, fragmentLength,
                     tigseq + tigbgn, tigend - tigbgn,
                     edlibNewAlignConfig(bandErrRate * fragmentLength, EDLIB_MODE_HW, EDLIB_TASK_PATH));

  if (align.alignmentLength > 0) {
    alignedErrRate = (double)align.editDistance / align.alignmentLength;
    aligned        = (alignedErrRate <= errorRate);
    if (verbose)
      fprintf(stderr, " - ALIGNED %.4f at %9d-%-9d\n", alignedErrRate, tigbgn + align.startLocations[0], tigbgn + align.endLocations[0]+1);
  } else {
    if (verbose)
      fprintf(stderr, "\n");
  }

  for (uint32 ii=0; ((ii < 4) && (aligned == false)); ii++) {
    tigbgn = max((int32)0,      tigbgn - 2 * padding);
    tigend = min((int32)tiglen, tigend + 2 * padding);

    bandErrRate += errorRate / 2;

    edlibFreeAlignResult(align);

    if (verbose)
      fprintf(stderr, "alignEdLib()--                    eRate %.4f at %9d-%-9d", bandErrRate, tigbgn, tigend);

    align = edlibAlign(fragment, strlen(fragment),
                       tigseq + tigbgn, tigend - tigbgn,
                       edlibNewAlignConfig(bandErrRate * fragmentLength, EDLIB_MODE_HW, EDLIB_TASK_PATH));

    if (align.alignmentLength > 0) {
      alignedErrRate = (double)align.editDistance / align.alignmentLength;
      aligned        = (alignedErrRate <= errorRate);
      if (verbose)
        fprintf(stderr, " - ALIGNED %.4f at %9d-%-9d\n", alignedErrRate, tigbgn + align.startLocations[0], tigbgn + align.endLocations[0]+1);
    } else {
      if (verbose)
        fprintf(stderr, "\n");
    }
  }

  if (aligned == false) {
    edlibFreeAlignResult(align);
    return(false);
  }

  char *tgtaln = new char [align.alignmentLength+1];
  char *qryaln = new char [align.alignmentLength+1];

  memset(tgtaln, 0, sizeof(char) * (align.alignmentLength+1));
  memset(qryaln, 0, sizeof(char) * (align.alignmentLength+1));

  edlibAlignmentToStrings(align.alignment,               //  Alignment
                          align.alignmentLength,         //    and length
                          align.startLocations[0],       //  tgtStart
                          align.endLocations[0]+1,       //  tgtEnd
                          0,                             //  qryStart
                          fragmentLength,                //  qryEnd
                          tigseq + tigbgn,               //  tgt sequence
                          fragment,                      //  qry sequence
                          tgtaln,                   //  output tgt alignment string
                          qryaln);                  //  output qry alignment string

  //  Populate the output.  AlnGraphBoost does not handle mismatch alignments, at all, so convert
  //  them to a pair of indel.

  uint32 nMatch = 0;

  for (uint32 ii=0; ii<align.alignmentLength; ii++)   //  Edlib guarantees aln[alignmentLength] == 0.
    if ((tgtaln[ii] != '-') &&
        (qryaln[ii] != '-') &&
        (tgtaln[ii] != qryaln[ii]))
      nMatch++;

  aln.start  = tigbgn + align.startLocations[0] + 1;   //  AlnGraphBoost expects 1-based positions.
  aln.end    = tigbgn + align.endLocations[0] + 1;     //  EdLib returns 0-based positions.

  aln.qstr   = new char [align.alignmentLength + nMatch + 1];
  aln.tstr   = new char [align.alignmentLength + nMatch + 1];

  for (uint32 ii=0, jj=0; ii<align.alignmentLength; ii++) {
    char  tc = tgtaln[ii];
    char  qc = qryaln[ii];

    if ((tc != '-') &&
        (qc != '-') &&
        (tc != qc)) {
      aln.tstr[jj] = '-';   aln.qstr[jj] = qc;    jj++;
      aln.tstr[jj] = tc;    aln.qstr[jj] = '-';   jj++;
    } else {
      aln.tstr[jj] = tc;    aln.qstr[jj] = qc;    jj++;
    }

    aln.length = jj;
  }

  aln.qstr[aln.length] = 0;
  aln.tstr[aln.length] = 0;

  delete [] tgtaln;
  delete [] qryaln;

  edlibFreeAlignResult(align);

  if (aln.end > tiglen)
    fprintf(stderr, "ERROR:  alignment from %d to %d, but tiglen is only %d\n", aln.start, aln.end, tiglen);
  assert(aln.end <= tiglen);

  return(true);
}



bool
unitigConsensus::initializeGenerate(tgTig                     *tig_,
                                    map<uint32, sqRead *>     *reads_,
                                    map<uint32, sqReadData *> *datas_) {

  _tig      = tig_;
  _numReads = _tig->numberOfChildren();

  if (initialize(reads_, datas_) == false) {
    fprintf(stderr, "Failed to initialize for tig %u with %u children\n", _tig->tigID(), _tig->numberOfChildren());
    return(false);
  }

  return(true);
}



bool
unitigConsensus::generatePBDAG(tgTig                     *tig_,
                               char                       aligner_,
                               map<uint32, sqRead *>     *reads_,
                               map<uint32, sqReadData *> *datas_) {

  if (initializeGenerate(tig_, reads_, datas_) == false)
    return(false);

  //  Build a quick consensus to align to.

  char   *tigseq = generateTemplateStitch();
  uint32  tiglen = strlen(tigseq);

  if (showAlgorithm())
    fprintf(stderr, "Generated template of length %d\n", tiglen);

  //  Compute alignments of each sequence in parallel

  if (showAlgorithm())
    fprintf(stderr, "Aligning reads.\n");

  dagAlignment *aligns = new dagAlignment [_numReads];
  uint32        pass = 0;
  uint32        fail = 0;

#pragma omp parallel for schedule(dynamic)
  for (uint32 ii=0; ii<_numReads; ii++) {
    abSequence  *seq      = getSequence(ii);
    bool         aligned  = false;

    assert(aligner_ == 'E');  //  Maybe later we'll have more than one aligner again.

    aligned = alignEdLib(aligns[ii],
                         _utgpos[ii],
                         seq->getBases(), seq->length(),
                         tigseq, tiglen,
                         (double)tiglen / _tig->_layoutLen,
                         _errorRate,
                         showAlgorithm());

    if (aligned == false) {
      if (showAlgorithm())
        fprintf(stderr, "generatePBDAG()--    read %7u FAILED\n", _utgpos[ii].ident());

      fail++;

      continue;
    }

    pass++;
  }

  if (showAlgorithm())
    fprintf(stderr, "Finished aligning reads.  %d failed, %d passed.\n", fail, pass);

  //  Construct the graph from the alignments.  This is not thread safe.

  if (showAlgorithm())
    fprintf(stderr, "Constructing graph\n");

  AlnGraphBoost ag(string(tigseq, tiglen));

  for (uint32 ii=0; ii<_numReads; ii++) {
    _cnspos[ii].setMinMax(aligns[ii].start, aligns[ii].end);

    if ((aligns[ii].start == 0) &&
        (aligns[ii].end   == 0))
      continue;

    ag.addAln(aligns[ii]);

    aligns[ii].clear();
  }

  delete [] aligns;

  if (showAlgorithm())
    fprintf(stderr, "Merging graph\n");

  //  Merge the nodes and call consensus
  ag.mergeNodes();

  if (showAlgorithm())
    fprintf(stderr, "Calling consensus\n");

  std::string cns = ag.consensus(1);

  delete [] tigseq;

  //  Save consensus

  resizeArrayPair(_tig->_gappedBases, _tig->_gappedQuals, 0, _tig->_gappedMax, (uint32) cns.length() + 1, resizeArray_doNothing);

  std::string::size_type len = 0;

  for (len=0; len<cns.size(); len++) {
    _tig->_gappedBases[len] = cns[len];
    _tig->_gappedQuals[len] = CNS_MIN_QV;
  }

  //  Terminate the string.

  _tig->_gappedBases[len] = 0;
  _tig->_gappedQuals[len] = 0;
  _tig->_gappedLen        = len;
  _tig->_layoutLen        = len;

  assert(len < _tig->_gappedMax);

  return(true);
}



bool
unitigConsensus::generateQuick(tgTig                     *tig_,
                               map<uint32, sqRead *>     *reads_,
                               map<uint32, sqReadData *> *datas_) {

  if (initializeGenerate(tig_, reads_, datas_) == false)
    return(false);

  //  Quick is just the template sequence, so one and done!

  char   *tigseq = generateTemplateStitch();
  uint32  tiglen = strlen(tigseq);

  //  Save consensus

  resizeArrayPair(_tig->_gappedBases, _tig->_gappedQuals, 0, _tig->_gappedMax, tiglen + 1, resizeArray_doNothing);

  for (uint32 ii=0; ii<tiglen; ii++) {
    _tig->_gappedBases[ii] = tigseq[ii];
    _tig->_gappedQuals[ii] = CNS_MIN_QV;
  }

  //  Set positions of all the reads.  We don't know anything and default to the incoming positions.

  for (uint32 ii=0; ii<_numReads; ii++)
    _cnspos[ii] = _utgpos[ii];

  //  Terminate the string.

  _tig->_gappedBases[tiglen] = 0;
  _tig->_gappedQuals[tiglen] = 0;
  _tig->_gappedLen           = tiglen;
  _tig->_layoutLen           = tiglen;

  delete [] tigseq;

  return(true);
}



bool
unitigConsensus::generateSingleton(tgTig                     *tig_,
                                   map<uint32, sqRead *>     *reads_,
                                   map<uint32, sqReadData *> *datas_) {

  if (initializeGenerate(tig_, reads_, datas_) == false)
    return(false);

  assert(_numReads == 1);

  //  Copy the single read to the tig sequence.

  abSequence  *seq      = getSequence(0);
  char        *fragment = seq->getBases();
  uint32       readLen  = seq->length();

  resizeArrayPair(_tig->_gappedBases, _tig->_gappedQuals, 0, _tig->_gappedMax, readLen + 1, resizeArray_doNothing);

  for (uint32 ii=0; ii<readLen; ii++) {
    _tig->_gappedBases[ii] = fragment[ii];
    _tig->_gappedQuals[ii] = CNS_MIN_QV;
  }

  //  Set positions of all the reads.

  _cnspos[0].setMinMax(0, readLen);

  //  Terminate the string.

  _tig->_gappedBases[readLen] = 0;
  _tig->_gappedQuals[readLen] = 0;
  _tig->_gappedLen            = readLen;
  _tig->_layoutLen            = readLen;

  return(true);
}





void
unitigConsensus::findCoordinates(void) {

  if (showPlacement()) {
    fprintf(stderr, "\n");
    fprintf(stderr, "TIG %u length %u\n", _tig->tigID(), _tig->length());
    fprintf(stderr, "\n");
  }

  //  Align each read to the final consensus.

  for (uint32 ii=0; ii<_numReads; ii++) {
    abSequence   *read    = getSequence(ii);
    char         *readSeq = read->getBases();
    uint32        readLen = read->length();

    if (showPlacement())
      fprintf(stderr, "\n");

    //  Align to the region of the consensus sequence the read claims to
    //  align to, extended by 5% of the read length on either end.  If it
    //  fails to align full length, make the extensions larger.

    int32  ext5 = readLen * 0.05;
    int32  ext3 = readLen * 0.05;
    double era  = _errorRate;

    int32  bgn=0, unaligned3=0;
    int32  end=0, unaligned5=0, len=0;

    while ((ext5 < readLen * 1.5) &&
           (ext3 < readLen * 1.5) &&
           (era  < 4 * _errorRate)) {
      bgn = max(0, _cnspos[ii].min() - ext5);
      end = min(_cnspos[ii].max() + ext3, (int32)_tig->length());
      len = end - bgn;   //  WAS: +1

      if (showPlacement())
        fprintf(stderr, "align read #%u length %u to %u-%u - extension %d %d error rate %.3f\n",
                ii, readLen, bgn, end, ext5, ext3, era);

      EdlibAlignResult align = edlibAlign(readSeq, readLen,
                                          _tig->bases() + bgn, len,
                                          edlibNewAlignConfig(readLen * era * 2, EDLIB_MODE_HW, EDLIB_TASK_LOC));

      //  If nothing aligned, make bigger and more leniant.

      if (align.numLocations == 0) {
        ext5 += readLen * 0.05;
        ext3 += readLen * 0.05;
        era  += 0.025;

        if (showPlacement())
          fprintf(stderr, "  NO ALIGNMENT - Increase extension to %d / %d and error rate to %.3f\n", ext5, ext3, era);

        edlibFreeAlignResult(align);
        bgn = end = len = 0;
        continue;
      }

      unaligned5 = align.startLocations[0];
      unaligned3 = len - (align.endLocations[0] + 1);   //  0-based position of last character in alignment.

      if (showPlacement())
        fprintf(stderr, "               - read %4u original %9u-%9u claimed %9u-%9u aligned %9u-%9u unaligned %d %d\n",
                ii,
                _utgpos[ii].min(), _utgpos[ii].max(),
                _cnspos[ii].min(), _cnspos[ii].max(),
                bgn, end,
                unaligned5, unaligned3);

      //  If bump the start, make bigger.

      if ((bgn > 0) && (unaligned5 <= 0)) {
        ext5 += readLen * 0.05;

        if (showPlacement())
          fprintf(stderr, "  BUMPED START - unaligned hangs %d %d - increase 5' extension to %d\n",
                  unaligned5, unaligned3, ext5);

        edlibFreeAlignResult(align);
        bgn = end = len = 0;
        continue;
      }

      //  If bump the end, make bigger.

      if ((end < _tig->length()) && (unaligned3 <= 0)) {
        ext3 += readLen * 0.05;

        if (showPlacement())
          fprintf(stderr, "  BUMPED END   - unaligned hangs %d %d - increase 3' extension to %d\n",
                  unaligned5, unaligned3, ext3);

        edlibFreeAlignResult(align);
        bgn = end = len = 0;
        continue;
      }

      //  Otherwise, SUCCESS!

      end = align.endLocations[0] + bgn + 1;     //  endLocation is 0-based position of last base in alignment.
      bgn = align.startLocations[0] + bgn;       //  startLocation is 0-based position of first base in alignment.

      edlibFreeAlignResult(align);

      break;   //  Stop looping over ext5, ext3 and era.
    }

    if ((bgn == 0) &&
        (end == 0)) {
      fprintf(stderr, "ERROR: FAILED.\n");
      exit(1);
    }
  }  //  Looping over reads

  //memcpy(tig->getChild(0), cnspos, sizeof(tgPosition) * numfrags);
}



void
unitigConsensus::findRawAlignments(void) {

#if 0
  for (uint32 ii=0; ii<_numReads; ii++) {
    fprintf(stderr, "read %4u original %9u-%9u aligned %9u-%9u\n",
            ii,
            _utgpos[ii].min(), _utgpos[ii].max(),
            _cnspos[ii].min(), _cnspos[ii].max());
  }
#endif
}



bool
unitigConsensus::generate(tgTig                     *tig_,
                          char                       algorithm_,
                          char                       aligner_,
                          map<uint32, sqRead *>     *reads_,
                          map<uint32, sqReadData *> *datas_) {
  bool  success = false;

  if      (tig_->numberOfChildren() == 1) {
    success = generateSingleton(tig_, reads_, datas_);
  }

  else if (algorithm_ == 'Q') {
    success = generateQuick(tig_, reads_, datas_);
  }

  else if (algorithm_ == 'P') {
    success = generatePBDAG(tig_, aligner_, reads_, datas_);

    if (success) {
      findCoordinates();
      findRawAlignments();
    }
  }

  else {
    fprintf(stderr, "Invalid algorithm.  How'd you do this?\n");
  }

  return(success);
}
