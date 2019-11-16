#include <catch2/catch.hpp>

#include "in_memory_backend.hpp"
#include "cache/cache.hpp"
#include "fs.hpp"

#include "testutils/tempdir.hpp"
#include "testutils/fuse_backend.hpp"

class TestEnvironment {
public:
    TestEnvironment():
        m_cache(m_cachedir.path()),
        m_fs(m_cache, m_backend),
        m_default_uid(getuid()),
        m_default_gid(getgid()),
        m_default_timestamp{.tv_sec = 1536390000, .tv_nsec = 20180908}
    {

    }

private:
    TemporaryDirectory m_cachedir;
    Dragonstash::Cache m_cache;
    Dragonstash::Backend::InMemoryFilesystem m_backend;
    Dragonstash::Filesystem m_fs;
    TestFuseBackend m_fuse;

    uid_t m_default_uid;
    gid_t m_default_gid;

    struct timespec m_default_timestamp;

public:
    [[nodiscard]] uid_t default_uid() const {
        return m_default_uid;
    }

    [[nodiscard]] gid_t default_gid() const {
        return m_default_gid;
    }

    [[nodiscard]] const struct timespec &default_timestamp() const {
        return m_default_timestamp;
    }

    [[nodiscard]] inline Dragonstash::Cache &cache() {
        return m_cache;
    }

    [[nodiscard]] inline Dragonstash::Backend::InMemoryFilesystem &backend() {
        return m_backend;
    }

    [[nodiscard]] inline Dragonstash::Filesystem &fs() {
        return m_fs;
    }

    [[nodiscard]] inline TestFuseBackend &fuse() {
        return m_fuse;
    }

    TestEnvironment &with_default_contents() {
        Dragonstash::Backend::Stat file_attr{
            .mode = S_IRUSR | S_IWUSR | S_IRGRP,
            .uid = m_default_uid,
            .gid = m_default_gid,
            .atime = m_default_timestamp,
            .mtime = m_default_timestamp,
            .ctime = m_default_timestamp,
        };

        Dragonstash::Backend::Stat dir_attr{
            .mode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP,
            .uid = m_default_uid,
            .gid = m_default_gid,
            .atime = m_default_timestamp,
            .mtime = m_default_timestamp,
            .ctime = m_default_timestamp,
        };

        using namespace Dragonstash::Backend::InMemory;
        m_backend.emplace<File>("README.md").update_attr(file_attr);
        auto &dir = m_backend.emplace<Directory>("books");
        dir.update_attr(dir_attr);
        dir.emplace<File>("Hitchhiker's Guide To The Galaxy.epub").update_attr(file_attr);
        dir.emplace<File>("The Elements of Style.epub").update_attr(file_attr);
        dir.emplace<Link>("best.epub", "Hitchhiker's Guide To The Galaxy.epub").update_attr(file_attr);
        return *this;
    }
};

void check_reply_type(const TestFuseRequest &req, TestFuseReplyType expected_type)
{
    REQUIRE(req.has_reply());
    if (req.reply_type() != expected_type) {
        if (req.reply_type() == TestFuseReplyType::ERROR) {
            auto err = std::get<TestFuseReplyErr>(req.reply_argv());
            CHECK(err == 0);
        }
    }
    REQUIRE(req.reply_type() == expected_type);
}

void check_reply_error(const TestFuseRequest &req, int err) {
    REQUIRE(req.has_reply());
    CHECK(req.reply_type() == TestFuseReplyType::ERROR);
    if (req.reply_type() == TestFuseReplyType::ERROR) {
        CHECK(std::get<TestFuseReplyErr>(req.reply_argv()) == err);
    }
}

SCENARIO("lookup") {
    TestEnvironment env;

    GIVEN("A filesystem with default contents") {
        env.with_default_contents();
        Dragonstash::Filesystem &fs = env.fs();

        WHEN("Requesting to look up an existing file") {
            auto req = env.fuse().new_request();
            fs.lookup(req.wrap(), Dragonstash::ROOT_INO, "README.md");

            THEN("The FS replies with an entry") {
                check_reply_type(req, TestFuseReplyType::ENTRY);

                AND_THEN("The entry has a distinct inode and is of regular file format") {
                    auto entry = std::get<TestFuseReplyEntry>(req.reply_argv());
                    CHECK(entry.ino != Dragonstash::ROOT_INO);
                    CHECK(entry.ino != Dragonstash::INVALID_INO);
                    CHECK((entry.attr.st_mode & S_IFMT) == S_IFREG);
                }

                AND_THEN("The entry has correct attributes") {
                    auto entry = std::get<TestFuseReplyEntry>(req.reply_argv());
                    CHECK(entry.attr.st_uid == env.default_uid());
                    CHECK(entry.attr.st_gid == env.default_gid());
                    CHECK((entry.attr.st_mode & S_IRWXU) == (S_IRUSR | S_IWUSR));
                    CHECK((entry.attr.st_mode & S_IRWXG) == (S_IRGRP));
                    CHECK((entry.attr.st_mode & S_IRWXO) == 0);
                    CHECK(entry.attr.st_mtim.tv_sec == env.default_timestamp().tv_sec);
                    CHECK(entry.attr.st_mtim.tv_nsec == env.default_timestamp().tv_nsec);
                }
            }

            AND_WHEN("Re-requesting the same file") {
                check_reply_type(req, TestFuseReplyType::ENTRY);
                auto entry_1 = std::get<TestFuseReplyEntry>(req.reply_argv());

                auto req = env.fuse().new_request();
                fs.lookup(req.wrap(), Dragonstash::ROOT_INO, "README.md");

                THEN("Its inode number is unchanged") {
                    check_reply_type(req, TestFuseReplyType::ENTRY);
                    auto entry_2 = std::get<TestFuseReplyEntry>(req.reply_argv());

                    CHECK(entry_1.ino == entry_2.ino);
                }
            }
        }

        WHEN("Requesting to look up a nonexistent file") {
            auto req = env.fuse().new_request();
            fs.lookup(req.wrap(), Dragonstash::ROOT_INO, "random name");

            THEN("The FS replies with ENOENT") {
                check_reply_error(req, ENOENT);
            }
        }

        WHEN("Requesting to look up a directory") {
            auto req = env.fuse().new_request();
            fs.lookup(req.wrap(), Dragonstash::ROOT_INO, "books");

            THEN("The FS replies with an entry") {
                check_reply_type(req, TestFuseReplyType::ENTRY);

                AND_THEN("The entry has a distinct inode and is of directory format") {
                    auto entry = std::get<TestFuseReplyEntry>(req.reply_argv());
                    CHECK(entry.ino != Dragonstash::ROOT_INO);
                    CHECK(entry.ino != Dragonstash::INVALID_INO);
                    CHECK((entry.attr.st_mode & S_IFMT) == S_IFDIR);
                }

                AND_THEN("The entry has correct attributes") {
                    auto entry = std::get<TestFuseReplyEntry>(req.reply_argv());
                    CHECK(entry.attr.st_uid == env.default_uid());
                    CHECK(entry.attr.st_gid == env.default_gid());
                    CHECK((entry.attr.st_mode & S_IRWXU) == (S_IRUSR | S_IWUSR | S_IXUSR));
                    CHECK((entry.attr.st_mode & S_IRWXG) == (S_IRGRP | S_IXGRP));
                    CHECK((entry.attr.st_mode & S_IRWXO) == 0);
                    CHECK(entry.attr.st_mtim.tv_sec == env.default_timestamp().tv_sec);
                    CHECK(entry.attr.st_mtim.tv_nsec == env.default_timestamp().tv_nsec);
                }
            }

            THEN("The inode is different from the inode of the file") {
                auto req_file = env.fuse().new_request();
                fs.lookup(req_file.wrap(), Dragonstash::ROOT_INO, "README.md");
                check_reply_type(req, TestFuseReplyType::ENTRY);
                check_reply_type(req_file, TestFuseReplyType::ENTRY);

                CHECK(std::get<TestFuseReplyEntry>(req.reply_argv()).ino !=
                        std::get<TestFuseReplyEntry>(req_file.reply_argv()).ino);
            }
        }

        WHEN("Setting the backend to be disconnected") {
            {
                auto req = env.fuse().new_request();
                fs.lookup(req.wrap(), Dragonstash::ROOT_INO, "README.md");
                check_reply_type(req, TestFuseReplyType::ENTRY);
            }

            env.backend().set_connected(false);

            AND_WHEN("Looking up an uncached inode") {
                auto req = env.fuse().new_request();
                fs.lookup(req.wrap(), Dragonstash::ROOT_INO, "books");

                THEN("EIO is returned") {
                    check_reply_error(req, EIO);
                }
            }

            AND_WHEN("Looking up a cached inode") {
                auto req = env.fuse().new_request();
                fs.lookup(req.wrap(), Dragonstash::ROOT_INO, "README.md");

                THEN("The entry is returned") {
                    check_reply_type(req, TestFuseReplyType::ENTRY);

                    auto entry = std::get<TestFuseReplyEntry>(req.reply_argv());
                    CHECK(entry.attr.st_mode == (S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP));
                    CHECK(entry.attr.st_uid == env.default_uid());
                    CHECK(entry.attr.st_gid == env.default_gid());
                }
            }
        }
    }
}

SCENARIO("opendir and readdir") {
    TestEnvironment env;

    GIVEN("A filesystem with default contents") {
        env.with_default_contents();
        Dragonstash::Filesystem &fs = env.fs();

        WHEN("Testing the synced flag of the root directory") {
            THEN("It is unset") {
                auto flag_result = env.cache().begin_ro().test_flag(Dragonstash::ROOT_INO, Dragonstash::InodeFlag::SYNCED);
                CHECK(flag_result.error() == 0);
                REQUIRE(flag_result);
                CHECK(!*flag_result);
            }
        }

        WHEN("Opening the root directory with opendir") {
            auto req = env.fuse().new_request();
            struct fuse_file_info fi{};
            fs.opendir(req.wrap(), Dragonstash::ROOT_INO, &fi);

            THEN("The call succeeds and returns using fuse_reply_open") {
                check_reply_type(req, TestFuseReplyType::OPEN);
            }

            THEN("The root directory is marked as synced") {
                auto flag_result = env.cache().begin_ro().test_flag(Dragonstash::ROOT_INO, Dragonstash::InodeFlag::SYNCED);
                CHECK(flag_result.error() == 0);
                REQUIRE(flag_result);
                CHECK(*flag_result);
            }

            THEN("Child directories are not marked as synced") {
                auto txn = env.cache().begin_ro();
                auto lookup_result = txn.lookup(Dragonstash::ROOT_INO, "books");
                CHECK(lookup_result.error() == 0);
                REQUIRE(lookup_result);

                auto flag_result = txn.test_flag(*lookup_result, Dragonstash::InodeFlag::SYNCED);
                CHECK(flag_result.error() == 0);
                REQUIRE(flag_result);
                CHECK(!*flag_result);
            }

            AND_WHEN("Setting the backend to disconnected") {
                env.backend().set_connected(false);

                AND_WHEN("Calling lookup on an existing entry") {
                    auto req = env.fuse().new_request();
                    fs.lookup(req, Dragonstash::ROOT_INO, "README.md");

                    THEN("The call succeds and it returns attributes") {
                        check_reply_type(req, TestFuseReplyType::ENTRY);
                        auto entry = std::get<TestFuseReplyEntry>(req.reply_argv());
                        CHECK(entry.attr.st_mode == (S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP));
                    }
                }

                AND_WHEN("Opening the root directory with opendir again") {
                    auto req = env.fuse().new_request();
                    struct fuse_file_info fi{};
                    fs.opendir(req.wrap(), Dragonstash::ROOT_INO, &fi);

                    THEN("The call succeeds and returns using fuse_reply_open") {
                        check_reply_type(req, TestFuseReplyType::OPEN);
                    }

                    AND_WHEN("Iterating it") {
                        check_reply_type(req, TestFuseReplyType::OPEN);
                        fuse_file_info fi = std::get<TestFuseReplyOpen>(req.reply_argv());

                        THEN("It returns data after dotdot") {
                            // XXX: this heavily relies on implementation
                            // details of the cache because we cannot
                            // deserialise the dir entry format used by fus

                            // To get the entry after dotdot, we have to ask
                            // starting at the offset equal to the parent inode
                            req = env.fuse().new_request();
                            const std::size_t size = 4096;
                            fs.readdir(req.wrap(), Dragonstash::ROOT_INO, size, Dragonstash::ROOT_INO, &fi);
                            check_reply_type(req, TestFuseReplyType::BUF);
                        }
                    }
                }

                AND_WHEN("Opening an uncached directory with opendir") {
                    auto req = env.fuse().new_request();
                    fs.lookup(req.wrap(), Dragonstash::ROOT_INO, "books");
                    check_reply_type(req, TestFuseReplyType::ENTRY);

                    ino_t dir_ino = std::get<TestFuseReplyEntry>(req.reply_argv()).ino;

                    req = env.fuse().new_request();
                    struct fuse_file_info fi{};
                    fs.opendir(req.wrap(), dir_ino, &fi);

                    THEN("The call succeeds") {
                        check_reply_type(req, TestFuseReplyType::OPEN);
                    }

                    AND_WHEN("Iterating it") {
                        check_reply_type(req, TestFuseReplyType::OPEN);
                        fuse_file_info fi = std::get<TestFuseReplyOpen>(req.reply_argv());

                        THEN("It returns EIO after dotdot") {
                            // XXX: this heavily relies on implementation
                            // details of the cache because we cannot
                            // deserialise the dir entry format used by fus

                            // To get the entry after dotdot, we have to ask
                            // starting at the offset equal to the parent inode
                            req = env.fuse().new_request();
                            const std::size_t size = 4096;
                            fs.readdir(req.wrap(), dir_ino, size, Dragonstash::ROOT_INO, &fi);
                            check_reply_error(req, EIO);
                        }
                    }
                }
            }

            AND_WHEN("Calling opendir again") {
                auto lookup_result_1 = env.cache().lookup(Dragonstash::ROOT_INO, "README.md");
                REQUIRE(lookup_result_1);
                auto lookup_result_2 = env.cache().lookup(Dragonstash::ROOT_INO, "books");
                REQUIRE(lookup_result_2);

                auto req = env.fuse().new_request();
                struct fuse_file_info fi{};
                fs.opendir(req.wrap(), Dragonstash::ROOT_INO, &fi);
                check_reply_type(req, TestFuseReplyType::OPEN);

                THEN("Inode numbers do not change") {
                    auto lookup_result_1_test = env.cache().lookup(Dragonstash::ROOT_INO, "README.md");
                    REQUIRE(lookup_result_1_test);
                    auto lookup_result_2_test = env.cache().lookup(Dragonstash::ROOT_INO, "books");
                    REQUIRE(lookup_result_2_test);

                    CHECK(*lookup_result_1 == *lookup_result_1_test);
                    CHECK(*lookup_result_2 == *lookup_result_2_test);
                }
            }
        }
    }
}
