#include "source/common/config/xds_symbol_table.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Config {
namespace {

TEST(XdsSymbolTableTest, LosslessRoundTrip) {
  auto table = std::make_shared<XdsSymbolTable>();

  std::vector<std::string> test_cases = {
      "simple_cluster",
      "cluster.with.dots",
      "cluster/with/slashes",
      "cluster_with_underscores-and-dashes",
      "xdstp://traffic-director-global.xds.googleapis.com/envoy.config.cluster.v3.Cluster/"
      "414017652406/default/"
      "cloud-internal-17-lb-europe-west1_cloud_17_ilb_1082309372360_4767540509642979888",
      "xdstp://auth/type/id?k1=v1&k2=v2:8080",
      "a/b/c/d/e/f/g",
      "://only_scheme",
      "",
  };

  for (const auto& original : test_cases) {
    auto storage = table->encode(original);
    XdsResourceName name = storage.resourceName();
    std::string decoded = table->toString(name);
    EXPECT_EQ(original, decoded);
  }
}

TEST(XdsSymbolTableTest, EqualityAndHash) {
  auto table = std::make_shared<XdsSymbolTable>();

  std::string str1 = "xdstp://authority/envoy.config.cluster.v3.Cluster/project/cluster1";
  std::string str2 = "xdstp://authority/envoy.config.cluster.v3.Cluster/project/cluster1";
  std::string str3 = "xdstp://authority/envoy.config.cluster.v3.Cluster/project/cluster2";

  auto storage1 = table->encode(str1);
  auto storage2 = table->encode(str2);
  auto storage3 = table->encode(str3);

  EXPECT_EQ(storage1.resourceName(), storage2.resourceName());
  EXPECT_NE(storage1.resourceName(), storage3.resourceName());

  EXPECT_EQ(absl::Hash<XdsResourceName>()(storage1.resourceName()),
            absl::Hash<XdsResourceName>()(storage2.resourceName()));
}

TEST(XdsSymbolTableTest, ByteCompressionRatio) {
  auto table = std::make_shared<XdsSymbolTable>();

  std::string long_name =
      "xdstp://traffic-director-global.xds.googleapis.com/envoy.config.cluster.v3.Cluster/"
      "414017652406/default/"
      "cloud-internal-17-lb-europe-west1_cloud_17_ilb_1082309372360_4767540509642979888";

  auto storage = table->encode(long_name);
  EXPECT_GT(long_name.size(), 170);
  // Encoded bytes should be roughly 40-50 bytes vs 184 bytes raw string
  EXPECT_LE(storage.resourceName().size(), 55);
  EXPECT_GT(storage.resourceName().size(), 10);

  // Compression ratio should be > 3x on raw bytes alone
  double ratio = static_cast<double>(long_name.size()) / storage.resourceName().size();
  EXPECT_GT(ratio, 3.0);
}

TEST(XdsSymbolTableTest, RefCountReclamation) {
  auto table = std::make_shared<XdsSymbolTable>();
  const size_t initial_active = table->numActiveSymbols();

  {
    auto storage = table->encode("unique_token");
    EXPECT_GT(table->numActiveSymbols(), initial_active);
  }

  // After storage goes out of scope, the token should be reclaimed from encode_map_
  EXPECT_EQ(table->numActiveSymbols(), initial_active);

  // Re-encoding should reuse symbol slot from free_list_
  const size_t symbols_before = table->numSymbols();
  auto storage2 = table->encode("another_token");
  // Total allocated symbols in table does not grow because it reused from free_list_
  EXPECT_LE(table->numSymbols(), symbols_before);
  EXPECT_EQ("another_token", table->toString(storage2.resourceName()));
}

TEST(XdsSymbolTableTest, StorageCopyMoveAndOrdering) {
  auto table = std::make_shared<XdsSymbolTable>();

  XdsResourceName empty_name;
  EXPECT_TRUE(empty_name.empty());
  EXPECT_EQ(empty_name.data(), nullptr);
  EXPECT_EQ(empty_name.size(), 0);

  XdsResourceNameStorage null_table_storage(nullptr, "ignored");
  EXPECT_EQ(null_table_storage.size(), 0);

  XdsResourceNameStorage s1(table, "alpha/one");
  EXPECT_GT(s1.size(), 0);
  EXPECT_EQ("alpha/one", table->toString(s1.resourceName()));

  // Copy constructor and copy assignment.
  XdsResourceNameStorage s2(s1);
  EXPECT_EQ(s1.resourceName(), s2.resourceName());
  XdsResourceNameStorage s3(table, "beta/two");
  EXPECT_TRUE(s1.resourceName() < s3.resourceName() || s3.resourceName() < s1.resourceName());
  EXPECT_FALSE(empty_name < XdsResourceName());
  EXPECT_TRUE(empty_name < s1.resourceName());

  s3 = s1;
  EXPECT_EQ(s1.resourceName(), s3.resourceName());
  s3 = null_table_storage;
  EXPECT_EQ(s3.size(), 0);

  // Move constructor and move assignment.
  XdsResourceNameStorage s4(std::move(s2));
  EXPECT_EQ("alpha/one", table->toString(s4.resourceName()));
  XdsResourceNameStorage s5(table, "gamma/three");
  s5 = std::move(s4);
  EXPECT_EQ("alpha/one", table->toString(s5.resourceName()));
}

TEST(XdsSymbolTableTest, MultiByteLeb128Symbols) {
  auto table = std::make_shared<XdsSymbolTable>();
  std::vector<XdsResourceNameStorage> storages;
  storages.reserve(200);
  for (int i = 0; i < 200; ++i) {
    storages.emplace_back(table->encode("token" + std::to_string(i) + "/suffix"));
  }
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ("token" + std::to_string(i) + "/suffix", table->toString(storages[i].resourceName()));
  }
}

} // namespace
} // namespace Config
} // namespace Envoy
