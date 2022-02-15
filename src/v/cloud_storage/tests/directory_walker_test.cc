/*
 * Copyright 2021 Vectorized, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/vectorizedio/redpanda/blob/master/licenses/rcl.md
 */

#include "bytes/iobuf.h"
#include "cloud_storage/recursive_directory_walker.h"
#include "seastarx.h"

#include <seastar/core/file.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <boost/test/tools/old/interface.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>

using namespace cloud_storage;
using namespace std::chrono_literals;

SEASTAR_THREAD_TEST_CASE(one_level) {
    cloud_storage::recursive_directory_walker _walker;
    const std::filesystem::path target_dir{"target_dir"};
    const std::filesystem::path file_path1{"target_dir/file1.txt"};
    const std::filesystem::path file_path2{"target_dir/file2.txt"};

    ss::recursive_touch_directory(target_dir.native()).get();

    auto flags = ss::open_flags::wo | ss::open_flags::create
                 | ss::open_flags::exclusive;
    auto file1 = ss::open_file_dma(file_path1.native(), flags).get();
    file1.close().get();

    // sleep is needed to ensure stable order of returned files. directory
    // walker returns files sorted by timestamp of creation, xfs stores creation
    // time with 1 second precision.
    ss::sleep(ss::lowres_clock::duration(1s)).get();

    auto file2 = ss::open_file_dma(file_path2.native(), flags).get();
    file2.close().get();

    auto result = _walker.walk(target_dir.native()).get();

    BOOST_REQUIRE_EQUAL(result.first, 0);
    BOOST_REQUIRE_EQUAL(result.second.size(), 2);
    BOOST_REQUIRE_EQUAL(result.second[0].path, file_path1.native());
    BOOST_REQUIRE_EQUAL(result.second[1].path, file_path2.native());
}

SEASTAR_THREAD_TEST_CASE(three_levels) {
    cloud_storage::recursive_directory_walker _walker;
    const std::filesystem::path target_dir{"target_dir1"};
    const std::filesystem::path file_path1{"target_dir1/a/file1.txt"};
    const std::filesystem::path file_path2{"target_dir1/file2.txt"};
    const std::filesystem::path file_path3{"target_dir1/b/c/file3.txt"};

    ss::recursive_touch_directory(target_dir.native()).get();
    ss::recursive_touch_directory("target_dir1/a").get();
    ss::recursive_touch_directory("target_dir1/b/c").get();

    auto flags = ss::open_flags::wo | ss::open_flags::create
                 | ss::open_flags::exclusive;
    auto file1 = ss::open_file_dma(file_path1.native(), flags).get();
    file1.close().get();
    // sleep is needed to ensure stable order of returned files. directory
    // walker returns files sorted by timestamp of creation, xfs stores creation
    // time with 1 second precision.
    ss::sleep(ss::lowres_clock::duration(1s)).get();

    auto file2 = ss::open_file_dma(file_path2.native(), flags).get();
    file2.close().get();
    ss::sleep(ss::lowres_clock::duration(1s)).get();

    auto file3 = ss::open_file_dma(file_path3.native(), flags).get();
    file3.close().get();
    ss::sleep(ss::lowres_clock::duration(1s)).get();

    auto result = _walker.walk(target_dir.native()).get();

    BOOST_REQUIRE_EQUAL(result.first, 0);
    BOOST_REQUIRE_EQUAL(result.second.size(), 3);
    BOOST_REQUIRE_EQUAL(result.second[0].path, file_path1.native());
    BOOST_REQUIRE_EQUAL(result.second[1].path, file_path2.native());
    BOOST_REQUIRE_EQUAL(result.second[2].path, file_path3.native());
}

SEASTAR_THREAD_TEST_CASE(no_files) {
    cloud_storage::recursive_directory_walker _walker;
    const std::filesystem::path target_dir2{"target_dir2"};
    const std::filesystem::path dir1{"target_dir2/a/b"};
    const std::filesystem::path dir2{"target_dir2/c"};

    ss::recursive_touch_directory(target_dir2.native()).get();
    ss::recursive_touch_directory(dir1.native()).get();
    ss::recursive_touch_directory(dir2.native()).get();

    auto result = _walker.walk(target_dir2.native()).get();

    BOOST_REQUIRE_EQUAL(result.first, 0);
    BOOST_REQUIRE_EQUAL(result.second.size(), 0);
}

SEASTAR_THREAD_TEST_CASE(empty_dir) {
    cloud_storage::recursive_directory_walker _walker;
    const std::filesystem::path target_dir3{"target_dir3"};
    ss::recursive_touch_directory(target_dir3.native()).get();

    auto result = _walker.walk(target_dir3.native()).get();

    BOOST_REQUIRE_EQUAL(result.first, 0);
    BOOST_REQUIRE_EQUAL(result.second.size(), 0);
}

void write_to_file(auto& target_file, uint64_t size) {
    ss::sstring data_string;
    data_string.resize(size, 'a');

    iobuf buf;
    buf.append(data_string.data(), data_string.length());

    auto input = make_iobuf_input_stream(std::move(buf));
    auto out = ss::make_file_output_stream(target_file).get();

    ss::copy(input, out).get();
    out.flush().get();
    out.close().get();
}

SEASTAR_THREAD_TEST_CASE(total_size_correct) {
    cloud_storage::recursive_directory_walker _walker;
    const std::filesystem::path target_dir4{"target_dir4"};
    const std::filesystem::path file_path1{"target_dir4/a/file1.txt"};
    const std::filesystem::path file_path2{"target_dir4/file2.txt"};
    const std::filesystem::path file_path3{"target_dir4/b/c/file3.txt"};

    ss::recursive_touch_directory(target_dir4.native()).get();
    ss::recursive_touch_directory("target_dir4/a").get();
    ss::recursive_touch_directory("target_dir4/b/c").get();

    auto flags = ss::open_flags::wo | ss::open_flags::create
                 | ss::open_flags::exclusive;
    auto file1 = ss::open_file_dma(file_path1.native(), flags).get();
    auto file2 = ss::open_file_dma(file_path2.native(), flags).get();
    auto file3 = ss::open_file_dma(file_path3.native(), flags).get();

    write_to_file(file1, 3412);
    write_to_file(file2, 8);
    write_to_file(file3, 342);

    file1.close().get();
    file2.close().get();
    file3.close().get();

    auto result = _walker.walk(target_dir4.native()).get();

    BOOST_REQUIRE_EQUAL(result.first, 3412 + 8 + 342);
    BOOST_REQUIRE_EQUAL(result.second.size(), 3);
}