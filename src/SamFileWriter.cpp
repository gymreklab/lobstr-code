/*
Copyright (C) 2011-2014 Melissa Gymrek <mgymrek@mit.edu>

This file is part of lobSTR.

lobSTR is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

lobSTR is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with lobSTR.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <boost/algorithm/string.hpp>

#include <map>
#include <string>
#include <vector>

#include "src/common.h"
#include "src/runtime_parameters.h"
#include "src/SamFileWriter.h"

using namespace std;

using BamTools::BamWriter;
using BamTools::RefData;
using BamTools::RefVector;
using BamTools::BamAlignment;
using BamTools::SamHeader;
using BamTools::SamReadGroup;
using BamTools::SamReadGroupDictionary;

SamFileWriter::SamFileWriter(const string& _filename,
                             const map<string, int>& _chrom_sizes) {
  chrom_sizes = _chrom_sizes;
  SamHeader header;
  header.Comments.push_back(user_defined_arguments);
  SamReadGroup rg(GetReadGroup());
  if (!read_group_sample.empty()) {
    rg.Sample = read_group_sample;
  }
  if (!read_group_library.empty()) {
    rg.Library = read_group_library;
  }
  header.ReadGroups.Add(rg);

  RefVector ref_vector;
  for (map<string, int>::const_iterator it = chrom_sizes.begin();
       it != chrom_sizes.end(); ++it) {
    const string& name = it->first;
    int length = it->second;
    RefData ref_data(name, length);
    ref_data.RefName = name;
    ref_vector.push_back(ref_data);
  }
  if (!writer.Open(_filename, header, ref_vector)) {
    PrintMessageDieOnError("Could not open bam file " + _filename, ERROR);
  }
}

void SamFileWriter::WriteRecord(const ReadPair& read_pair) {
  const int& aligned_read_num = read_pair.aligned_read_num;
  const int& paired_dist = read_pair.treat_as_paired ?
    abs(read_pair.reads.at(aligned_read_num).read_start-
        read_pair.reads.at(1-aligned_read_num).read_start) : -1;
  // Update run info for this read
  run_info.num_aligned_reads++;
  run_info.total_insert += paired_dist;
  if (read_pair.treat_as_paired) {
    run_info.num_aligned_reads++;
    run_info.num_mates++;
  } 
  if (!read_pair.treat_as_paired && paired) {
    run_info.num_stitched++;
  } else {
    run_info.num_single++;
  }
  if (read_pair.reads.at(aligned_read_num).reverse) {
    run_info.num_reverse++;
  }
  if (read_pair.reads.at(aligned_read_num).diffFromRef %
      static_cast<int>(read_pair.reads.at(aligned_read_num).repseq.length()) != 0) {
    run_info.num_nonunit++;
  }
  // Write aligned read
  if (align_debug) {
    PrintMessageDieOnError("[SamFileWriter.cpp]: Writing alignment output", DEBUG);
  }
		  
  BamAlignment bam_alignment;
  BamAlignment mate_alignment;
  bool str_alignment_is_first = false; // Does the STR alignment have a smaller start coord than mate
  bam_alignment.Name = StandardizeReadID(read_pair.reads.at(aligned_read_num).ID, read_pair.treat_as_paired);

  // Flags
  bam_alignment.SetIsDuplicate(false);
  bam_alignment.SetIsFailedQC(false);
  bam_alignment.SetIsMapped(true);
  bam_alignment.SetIsPrimaryAlignment(true);
  bam_alignment.SetIsReverseStrand(read_pair.reads.
                                   at(aligned_read_num).reverse);
  if (read_pair.treat_as_paired) {
    bam_alignment.SetIsPaired(true);
    bam_alignment.SetIsMateMapped(true);
    bam_alignment.SetIsProperPair(true);
    bam_alignment.SetIsMateReverseStrand(read_pair.reads.
					 at(1-aligned_read_num).reverse);
    bam_alignment.SetIsFirstMate(true);
    bam_alignment.SetIsSecondMate(false);
    if (read_pair.reads.at(aligned_read_num).read_start <
	read_pair.reads.at(1-aligned_read_num).read_start) {
      str_alignment_is_first = true;
    }
  } else {
    if (align_debug) {
      PrintMessageDieOnError("[SamFileWriter.cpp]: Alignment is single end", DEBUG);
    }
    bam_alignment.SetIsPaired(false);
    bam_alignment.SetIsProperPair(false);    
    bam_alignment.SetIsFirstMate(false);
    bam_alignment.SetIsSecondMate(false);
    bam_alignment.SetIsMateMapped(true);
    bam_alignment.SetIsMateReverseStrand(false);
  }
  bam_alignment.Position = read_pair.reads.at(aligned_read_num).read_start;
  bam_alignment.Qualities = read_pair.reads.
    at(aligned_read_num).quality_scores;
  if (read_pair.alternate_mappings.empty()) {
    bam_alignment.MapQuality = 255;
  } else {
    bam_alignment.MapQuality = 0;
  }
  if (read_pair.reads.at(aligned_read_num).reverse) {
    bam_alignment.QueryBases =
      reverseComplement(read_pair.reads.at(aligned_read_num).nucleotides);
  } else {
    bam_alignment.QueryBases =
      read_pair.reads.at(aligned_read_num).nucleotides;
  }

  // rname (ref id)
  int ref_id = -1;
  size_t i = 0;
  for (map<string, int>::const_iterator it = chrom_sizes.begin();
       it != chrom_sizes.end(); ++it) {
    if (it->first == read_pair.reads.at(aligned_read_num).chrom) {
      ref_id = i;
    }
    ++i;
  }
  if (ref_id == -1) {
    PrintMessageDieOnError("[SamFileWriter.cpp]: problem setting refid", ERROR);
  }
  bam_alignment.RefID = ref_id;
  if (read_pair.treat_as_paired) {
    bam_alignment.MateRefID = ref_id; // always will map to same chromosome
    bam_alignment.MatePosition = read_pair.reads.at(1-aligned_read_num).read_start;
  }
  // cigar
  vector<BamTools::CigarOp> cigar_data;
  for (i = 0; i < read_pair.reads.at(aligned_read_num).cigar.size(); i++) {
    char cigar_type = read_pair.reads.
      at(aligned_read_num).cigar.at(i).cigar_type;
    int num = read_pair.reads.at(aligned_read_num).cigar.at(i).num;
    BamTools::CigarOp cigar_op(cigar_type, num);
    cigar_data.push_back(cigar_op);
  }
  bam_alignment.CigarData = cigar_data;

  // write user flags giving repeat information
  // XA: alternate alignments
  if (!read_pair.alternate_mappings.empty()) {
    bam_alignment.AddTag("XA", "Z", read_pair.alternate_mappings);
  }
  // XO: other spanned STRs
  if (!read_pair.other_spanned_strs.empty()) {
    bam_alignment.AddTag("XO", "Z", read_pair.other_spanned_strs);
  }
  // XS: start pos of matching STR
  bam_alignment.AddTag("XS", "i", read_pair.reads.
                       at(aligned_read_num).msStart);
  // XE: end pos of matching STR
  bam_alignment.AddTag("XE", "i", read_pair.reads.
                       at(aligned_read_num).msEnd);
  // XR: STR repeat
  bam_alignment.AddTag("XR", "Z", read_pair.reads.
                       at(aligned_read_num).repseq);
  // XD: nuc diff compared to ref
  bam_alignment.AddTag("XD", "i", read_pair.reads.
                       at(aligned_read_num).diffFromRef);
  // XC: ref copy number
  bam_alignment.AddTag("XC", "f", read_pair.reads.
                       at(aligned_read_num).refCopyNum);
  // XG: repeat region
  bam_alignment.AddTag("XG", "Z", read_pair.reads.
		       at(aligned_read_num).detected_ms_nuc);
  // XX: stitched
  bam_alignment.AddTag("XX", "i", static_cast<int>
                       (!read_pair.treat_as_paired && paired));

  // XM: distance between read and mate start pos
  bam_alignment.AddTag("XM", "i", paired_dist);
  // XN: name of STR repeat
  if (!read_pair.reads.at(aligned_read_num).name.empty()) {
    bam_alignment.AddTag("XN", "Z", read_pair.reads.at(aligned_read_num).name);
  }
  // XQ: alignment quality score
  bam_alignment.AddTag("XQ", "i", read_pair.reads.at(aligned_read_num).mapq);
  // RG: read group
  bam_alignment.AddTag("RG", "Z", GetReadGroup());
  // NM: edit distance to reference
  bam_alignment.AddTag("NM", "i", read_pair.reads.at(aligned_read_num).edit_dist);

  // Write mate pair
  if (read_pair.treat_as_paired) {
    mate_alignment.Name = StandardizeReadID(read_pair.reads.at(1-aligned_read_num).ID, read_pair.treat_as_paired);
    mate_alignment.SetIsPaired(true);
    mate_alignment.SetIsDuplicate(false);
    mate_alignment.SetIsFailedQC(false);
    mate_alignment.SetIsFirstMate(false);
    mate_alignment.SetIsSecondMate(true);
    mate_alignment.SetIsMateReverseStrand(read_pair.reads.
					  at(aligned_read_num).reverse);
    mate_alignment.SetIsProperPair(true);
    mate_alignment.SetIsMapped(true);
    mate_alignment.SetIsMateMapped(true);
    mate_alignment.SetIsPrimaryAlignment(true);
    mate_alignment.SetIsReverseStrand(read_pair.reads.
				      at(1-aligned_read_num).reverse);
    mate_alignment.Position =
      read_pair.reads.at(1-aligned_read_num).read_start;
    if (read_pair.alternate_mappings.empty()) {
      mate_alignment.MapQuality = 255;
    } else {
      mate_alignment.MapQuality = 0;
    }
    mate_alignment.Qualities =
      read_pair.reads.at(1-aligned_read_num).quality_scores;
    if (read_pair.reads.at(1-aligned_read_num).reverse) {
      mate_alignment.QueryBases =
	reverseComplement(read_pair.reads.at(1-aligned_read_num).nucleotides);
    } else {
      mate_alignment.QueryBases =
	read_pair.reads.at(1-aligned_read_num).nucleotides;
    }
    mate_alignment.RefID = ref_id;
    mate_alignment.MateRefID = ref_id;
    mate_alignment.MatePosition = read_pair.reads.at(aligned_read_num).read_start;
    // cigar
    vector<BamTools::CigarOp> cigar_data;
    for (i = 0; i < read_pair.reads.at(1-aligned_read_num).cigar.size();
	 i++) {
      char cigar_type = read_pair.reads.
	at(1-aligned_read_num).cigar.at(i).cigar_type;
      int num = read_pair.reads.at(1-aligned_read_num).cigar.at(i).num;
      BamTools::CigarOp cigar_op(cigar_type, num);
      cigar_data.push_back(cigar_op);
    }
    mate_alignment.CigarData = cigar_data;
    
    // write user flags giving repeat information
    // XS: start pos of matching STR
    mate_alignment.AddTag("XS", "i", read_pair.reads.
			  at(aligned_read_num).msStart);
    // XE: end pos of matching STR
    mate_alignment.AddTag("XE", "i", read_pair.reads.
			  at(aligned_read_num).msEnd);
    // XR: STR repeat
    mate_alignment.AddTag("XR", "Z", read_pair.reads.
			  at(aligned_read_num).repseq);
    // XC: ref copy number
    mate_alignment.AddTag("XC", "f", read_pair.reads.
			  at(aligned_read_num).refCopyNum);
    // XN: name of STR repeat
    if (!read_pair.reads.at(aligned_read_num).name.empty()) {
      mate_alignment.AddTag("XN", "Z", read_pair.reads.
			    at(aligned_read_num).name);
    }
    // XQ: alignment quality score
    mate_alignment.AddTag("XQ", "f", read_pair.reads.at(aligned_read_num).mapq);
    // RG: read group
    mate_alignment.AddTag("RG", "Z", GetReadGroup());
    // NM: edit distance to reference
    mate_alignment.AddTag("NM", "i", read_pair.reads.at(1-aligned_read_num).edit_dist);
    // XA: Alternate alignment info
    if (!read_pair.alternate_mappings.empty()) {
      mate_alignment.AddTag("XA", "Z", read_pair.alternate_mappings);
    }
  }
  if (read_pair.treat_as_paired) {
    if (str_alignment_is_first) {
      writer.SaveAlignment(bam_alignment);
      writer.SaveAlignment(mate_alignment);
    } else {
      writer.SaveAlignment(mate_alignment);
      writer.SaveAlignment(bam_alignment);
    }
  } else {
    if (align_debug) {
      PrintMessageDieOnError("[SamFileWriter.cpp]: Done writing single end alignment", DEBUG);
    }
    writer.SaveAlignment(bam_alignment);
  }
}

void SamFileWriter::WriteAllelotypeRead(const BamTools::BamAlignment& aln, const std::string& filter,
                                        const std::string& chrom, const int& str_start, const int& str_end,
                                        const std::string& repseq, const int& allele) {
  // Make a new BAM aligment
  BamAlignment bam_alignment;
  // Only set basic fields
  bam_alignment.Name = aln.Name;
  if (chrom == "") {
    bam_alignment.RefID = 0;
  } else {
    int ref_id = -1;
    size_t i = 0;
    for (map<string, int>::const_iterator it = chrom_sizes.begin();
         it != chrom_sizes.end(); ++it) {
      if (it->first == chrom) {
        ref_id = i;
      }
      ++i;
    }
    if (ref_id == -1) {
      PrintMessageDieOnError("[SamFileWriter.cpp]: problem setting refid", ERROR);
    }
    bam_alignment.RefID = ref_id;
  }
  bam_alignment.Position = aln.Position;
  bam_alignment.QueryBases = aln.QueryBases;
  bam_alignment.Qualities = aln.Qualities;
  bam_alignment.MapQuality = aln.MapQuality;
  bam_alignment.CigarData = aln.CigarData;
  // Set tags
  bam_alignment.AddTag("XF", "Z", filter);
  if (str_start != -1) {
    stringstream locus_str;
    locus_str << chrom << ":" << str_start << "-" << str_end << ":" << repseq;
    bam_alignment.AddTag("XL", "Z", locus_str.str());
    if (filter == "PASS") {
      bam_alignment.AddTag("XD", "i", allele);
    }
  }
  writer.SaveAlignment(bam_alignment);
}

std::string SamFileWriter::StandardizeReadID(const std::string& readid, bool paired) {
  vector<std::string> strs;
  boost::split(strs, readid, boost::is_any_of("\t "));
  if (strs.size() == 0) {
    PrintMessageDieOnError("[SamFileWriter]: Error parsing read id", ERROR);
  }
  std::string newid = strs.at(0);
  std::string sub;
  if (paired) {
    if (newid.size() >= 2) {
      sub = newid.substr(newid.size()-2, 2);
      if (sub == "/1" || sub == "/2") {
	newid = newid.substr(0, newid.size() - 2);
      }
    }
  }
  return newid;
}

SamFileWriter::~SamFileWriter() {
  writer.Close();
}

