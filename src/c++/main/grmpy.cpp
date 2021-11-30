// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Paragraph
// Copyright (c) 2016-2019 Illumina, Inc.
// All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//		http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied
// See the License for the specific language governing permissions and limitations
//
//

/**
 * graph genotyper for graph models
 *
 * \author Sai Chen & Peter Krusche & Egor Dolzhenko
 * \email pkrusche@illumina.com & edolzhenko@illumina.com
 *
 */

#include <fstream>
#include <iostream>
#include <string>

#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "spdlog/spdlog.h"

#include "grmpy/Parameters.hh"
#include "grmpy/Workflow.hh"

#include "common/Error.hh"
#include "common/Program.hh"

// define to dump argc/argv
// #define GRMPY_TRACE

using std::string;
namespace po = boost::program_options;

using namespace grmpy;

class Options : public common::Options
{
public:
    Options();

    common::Options::Action parse(const char* moduleName, int argc, const char* argv[]);
    void postProcess(boost::program_options::variables_map& vm) override;

    string reference_path;
    std::vector<std::string> graph_spec_paths;
    string output_file_path;
    string output_folder_path;
    genotyping::Samples manifest;
    string genotyping_parameter_path;
    int sample_threads = std::thread::hardware_concurrency();
    int max_reads_per_event = 10000;
    float bad_align_frac = 0.8f;
    bool path_sequence_matching = false;
    bool graph_sequence_matching = true;
    bool klib_sequence_matching = false;
    bool kmer_sequence_matching = false;
    int bad_align_uniq_kmer_len = 0;
    string alignment_output_path;
    bool infer_read_haplotypes = false;

    bool gzip_output = false;
    bool progress = true;

    std::string usagePrefix() const override
    {
        return "grmpy -r <reference> -g <graphs> -m <manifest> [optional arguments]";
    }
};

Options::Options()
{
    // clang-format off
    namedOptions_.add_options()
            ("reference,r", po::value<string>(&reference_path), "Reference genome fasta file.")
            ("graph-spec,g", po::value<std::vector<string>>()->multitoken(), "JSON file(s) describing the graph(s)")
            ("genotyping-parameters,G", po::value<string>(&genotyping_parameter_path), "JSON file with genotyping model parameters")
            ("manifest,m", po::value<string>(), "Manifest of samples with path and bam stats.")
            ("output-file,o", po::value<string>(&output_file_path),
             "Output file name. Will output to stdout if omitted or '-'.")
            ("output-folder,O", po::value<string>(&output_folder_path),
             "Output folder path. paragraph will attempt to create "
             "the folder but not the entire path. Will output to stdout if neither of output-file or "
             "output-folder provided. If specified, paragraph will produce one output file for each "
             "input file bearing the same name.")
            ("alignment-output-folder,A", po::value<string>(&alignment_output_path)->default_value(alignment_output_path),
             "Output folder for alignments. Note these can become very large and are only required"
             "for curation / visualisation or faster reanalysis.")
            ("infer-read-haplotypes",
             po::value<bool>(&infer_read_haplotypes)->default_value(infer_read_haplotypes)->implicit_value(true),
             "Infer haplotype paths using read and fragment information.")
            ("max-reads-per-event,M", po::value<int>(&max_reads_per_event)->default_value(max_reads_per_event),
             "Maximum number of reads to process for a single event.")
            ("bad-align-frac", po::value<float>(&bad_align_frac)->default_value(bad_align_frac),
             "Fraction of read that needs to be mapped in order for it to be used.")
            ("path-sequence-matching",
             po::value<bool>(&path_sequence_matching)->default_value(path_sequence_matching),
             "Enables alignment to paths")
            ("graph-sequence-matching",
             po::value<bool>(&graph_sequence_matching)->default_value(graph_sequence_matching),
             "Enables smith waterman graph alignment")
            ("klib-sequence-matching",
             po::value<bool>(&klib_sequence_matching)->default_value(klib_sequence_matching),
             "Use klib smith-waterman aligner.")
            ("kmer-sequence-matching",
             po::value<bool>(&kmer_sequence_matching)->default_value(kmer_sequence_matching),
             "Use kmer aligner.")
            ("bad-align-uniq-kmer-len", po::value<int>(&bad_align_uniq_kmer_len)->default_value(bad_align_uniq_kmer_len),
             "Kmer length for uniqueness check during read filtering.")
            ("sample-threads,t", po::value<int>(&sample_threads)->default_value(sample_threads),
             "Number of threads for parallel sample processing.")
            ("gzip-output,z", po::value<bool>(&gzip_output)->default_value(gzip_output)->implicit_value(true),
             "gzip-compress output files. If -O is used, output file names are appended with .gz")
            ("progress", po::value<bool>(&progress)->default_value(progress)->implicit_value(true))
            ;
    // clang-format on
}

/**
 * \brief remembers the original argv array and hands over to the base implementation
 */
Options::Action Options::parse(const char* moduleName, int argc, const char* argv[])
{
    const std::vector<std::string> allOptions(argv, argv + argc);
#ifdef GRMPY_TRACE
    LOG()->info("argc: {} argv: {}", argc, boost::join(allOptions, " "));
#endif
    common::Options::Action ret = common::Options::parse(moduleName, argc, argv);
    return ret;
}

void Options::postProcess(boost::program_options::variables_map& vm)
{
    std::shared_ptr<spdlog::logger> logger = LOG();

    if (vm.count("reference"))
    {
        logger->info("Reference path: {}", reference_path);
        assertFileExists(reference_path);
    }
    else
    {
        error("Error: Reference genome path is missing.");
    }

    if (vm.count("graph-spec") != 0u)
    {
        graph_spec_paths = vm["graph-spec"].as<std::vector<string>>();
        logger->info("Graph spec: {}", boost::join(graph_spec_paths, ","));
        assertFilesExist(graph_spec_paths.begin(), graph_spec_paths.end());
        if (vm.count("output-folder"))
        {
            // If we're to produce individual output files per input, the input file
            // paths must have unique file names.
            assertFileNamesUnique(graph_spec_paths.begin(), graph_spec_paths.end());
        }
    }

    if (output_file_path.empty() && !vm.count("output-folder"))
    {
        output_file_path = "-";
    }

    if (!output_folder_path.empty())
    {
        logger->info("Output folder path: {}", output_folder_path);
        boost::filesystem::create_directory(output_folder_path);
    }

    if (!alignment_output_path.empty())
    {
        const bool force = alignment_output_path[0] == '!';
        if (force)
        {
            alignment_output_path = alignment_output_path.substr(1);
        }

        logger->info("Alignment output folder: {}", alignment_output_path);

        const bool dir_already_exists = boost::filesystem::is_directory(alignment_output_path);
        if (!force && dir_already_exists)
        {
            error("Alignment output folder %s already exists.", alignment_output_path.c_str());
        }

        if (!dir_already_exists)
        {
            boost::filesystem::create_directory(alignment_output_path);
        }
    }

    if (vm.count("manifest"))
    {
        const string manifest_path = vm["manifest"].as<string>();
        logger->info("Manifest path: {}", manifest_path);
        assertFileExists(manifest_path);
        manifest = genotyping::loadManifest(manifest_path);
        if (graph_spec_paths.empty())
        {
            // If no graphs given, all manifest samples must have paragraph column set
            for (const genotyping::SampleInfo& sample : manifest)
            {
                if (sample.get_alignment_data().isNull())
                {
                    error(
                        "Error: No graphs given on the command line and sample '%s' has empty paragraph "
                        "column in the manifest.",
                        sample.sample_name().c_str());
                }
            }
        }
        else if (1 < graph_spec_paths.size())
        {
            for (const genotyping::SampleInfo& sample : manifest)
            {
                if (!sample.get_alignment_data().isNull())
                {
                    error(
                        "ERROR: Pre-aligned samples are allowed only when genotyping for a single variant. %d "
                        "graphs provided.",
                        graph_spec_paths.size());
                }
            }
        }
    }
    else
    {
        error("Error: Manifest file is missing.");
    }
}

static void runGrmpy(const Options& options)
{
    Parameters parameters(
        options.sample_threads, options.max_reads_per_event, options.bad_align_frac, options.path_sequence_matching,
        options.graph_sequence_matching, options.klib_sequence_matching, options.kmer_sequence_matching,
        options.bad_align_uniq_kmer_len, options.alignment_output_path, options.infer_read_haplotypes);
    std::cerr << "starting workflow" << std::endl
    grmpy::Workflow workflow(
        options.graph_spec_paths, options.genotyping_parameter_path, options.manifest, options.output_file_path,
        options.output_folder_path, options.gzip_output, parameters, options.reference_path, options.progress);
    workflow.run();
}

int main(int argc, const char* argv[])
{
    common::run(runGrmpy, "Genotyping", argc, argv);
    return 0;
}
