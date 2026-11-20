#include "FrameReader.hpp"
#include "Homography.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace vfdt
{

namespace
{

constexpr int binarize_tile = 16;
constexpr int min_cluster_hits = 3;
constexpr double min_module_share = 0.6;
constexpr double min_contrast = 40.0;
constexpr double blend_low = 0.25;
constexpr double blend_high = 0.75;
constexpr double sample_spread = 0.2;

// For each rotation of the recording, which image corner (top-left, top-right, bottom-left, bottom-right) holds
// each frame corner, in the same order.
constexpr std::array<std::array<std::size_t, 4>, 4> corner_rotations{{
    {0, 1, 2, 3},
    {1, 3, 0, 2},
    {3, 2, 1, 0},
    {2, 0, 3, 1},
}};

using Color = std::array<double, 3>;

struct BinaryImage
{
    int width{};
    int height{};
    std::vector<std::uint8_t> dark;
};

struct FinderCandidate
{
    double x{};
    double y{};
    double module{};
};

struct FinderCluster
{
    double sumX{};
    double sumY{};
    double sumModule{};
    int hits{};
};

BinaryImage Binarize(const Image& image)
{
    const int width = image.width;
    const int height = image.height;
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> luma(pixelCount);
    for (std::size_t i = 0; i < pixelCount; ++i)
    {
        const std::uint8_t* pixel = image.pixels.data() + 3 * i;
        luma[i] = static_cast<std::uint8_t>((77U * pixel[0] + 150U * pixel[1] + 29U * pixel[2]) >> 8);
    }

    // Threshold each pixel against the mean of a wide window of tiles around it.
    const int tilesX = (width + binarize_tile - 1) / binarize_tile;
    const int tilesY = (height + binarize_tile - 1) / binarize_tile;
    const std::size_t stride = static_cast<std::size_t>(tilesX) + 1;
    std::vector<std::uint64_t> tileSums(static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY), 0);
    std::vector<std::uint64_t> tileCounts(tileSums.size(), 0);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t tile = static_cast<std::size_t>(y / binarize_tile) * static_cast<std::size_t>(tilesX) +
                                     static_cast<std::size_t>(x / binarize_tile);
            tileSums[tile] += luma[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
            ++tileCounts[tile];
        }
    }
    std::vector<std::uint64_t> sumTable(stride * (static_cast<std::size_t>(tilesY) + 1), 0);
    std::vector<std::uint64_t> countTable(sumTable.size(), 0);
    for (int ty = 0; ty < tilesY; ++ty)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            const std::size_t tile = static_cast<std::size_t>(ty) * static_cast<std::size_t>(tilesX) + static_cast<std::size_t>(tx);
            const std::size_t here = (static_cast<std::size_t>(ty) + 1) * stride + static_cast<std::size_t>(tx) + 1;
            const std::size_t up = here - stride;
            sumTable[here] = tileSums[tile] + sumTable[up] + sumTable[here - 1] - sumTable[up - 1];
            countTable[here] = tileCounts[tile] + countTable[up] + countTable[here - 1] - countTable[up - 1];
        }
    }
    const int radius = std::max(2, std::max(width, height) / (12 * binarize_tile));
    std::vector<double> thresholds(tileSums.size(), 0.0);
    for (int ty = 0; ty < tilesY; ++ty)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            const std::size_t x0 = static_cast<std::size_t>(std::max(0, tx - radius));
            const std::size_t x1 = static_cast<std::size_t>(std::min(tilesX, tx + radius + 1));
            const std::size_t y0 = static_cast<std::size_t>(std::max(0, ty - radius));
            const std::size_t y1 = static_cast<std::size_t>(std::min(tilesY, ty + radius + 1));
            const std::uint64_t sum = sumTable[y1 * stride + x1] - sumTable[y0 * stride + x1] - sumTable[y1 * stride + x0] + sumTable[y0 * stride + x0];
            const std::uint64_t count = countTable[y1 * stride + x1] - countTable[y0 * stride + x1] - countTable[y1 * stride + x0] + countTable[y0 * stride + x0];
            thresholds[static_cast<std::size_t>(ty) * static_cast<std::size_t>(tilesX) + static_cast<std::size_t>(tx)] =
                static_cast<double>(sum) / static_cast<double>(count);
        }
    }

    BinaryImage binary{width, height, std::vector<std::uint8_t>(pixelCount, 0)};
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            const std::size_t tile = static_cast<std::size_t>(y / binarize_tile) * static_cast<std::size_t>(tilesX) +
                                     static_cast<std::size_t>(x / binarize_tile);
            binary.dark[index] = luma[index] < thresholds[tile] ? 1 : 0;
        }
    }
    return binary;
}

bool CheckRatio(const std::array<int, 5>& runs, double& module)
{
    int total = 0;
    for (const int run : runs)
    {
        if (run == 0)
        {
            return false;
        }
        total += run;
    }
    if (total < 7)
    {
        return false;
    }
    module = total / 7.0;
    const double variance = module / 2.0;
    return std::abs(module - runs[0]) < variance && std::abs(module - runs[1]) < variance &&
           std::abs(3.0 * module - runs[2]) < 3.0 * variance && std::abs(module - runs[3]) < variance &&
           std::abs(module - runs[4]) < variance;
}

int PixelAt(const BinaryImage& binary, int x, int y)
{
    if (x < 0 || y < 0 || x >= binary.width || y >= binary.height)
    {
        return -1;
    }
    return binary.dark[static_cast<std::size_t>(y) * static_cast<std::size_t>(binary.width) + static_cast<std::size_t>(x)];
}

// Measures dark-light-DARK-light-dark runs through (x0, y0) along (dx, dy); center is the core middle on that axis.
bool MeasureCross(const BinaryImage& binary, int x0, int y0, int dx, int dy, int limit, std::array<int, 5>& runs, double& center)
{
    const auto value = [&](int k)
    {
        return PixelAt(binary, x0 + k * dx, y0 + k * dy);
    };
    runs = {};
    if (value(0) != 1)
    {
        return false;
    }

    int k = 0;
    while (value(-k) == 1 && runs[2] <= limit)
    {
        ++runs[2];
        ++k;
    }
    const int coreFirst = -(k - 1);
    while (value(-k) == 0 && runs[1] <= limit)
    {
        ++runs[1];
        ++k;
    }
    while (value(-k) == 1 && runs[0] <= limit)
    {
        ++runs[0];
        ++k;
    }
    k = 1;
    while (value(k) == 1 && runs[2] <= limit)
    {
        ++runs[2];
        ++k;
    }
    const int coreLast = k - 1;
    while (value(k) == 0 && runs[3] <= limit)
    {
        ++runs[3];
        ++k;
    }
    while (value(k) == 1 && runs[4] <= limit)
    {
        ++runs[4];
        ++k;
    }
    for (const int run : runs)
    {
        if (run > limit)
        {
            return false;
        }
    }

    const int base = dx != 0 ? x0 : y0;
    center = base + (coreFirst + coreLast + 1) / 2.0;
    return true;
}

std::vector<FinderCandidate> ScanFinderCandidates(const BinaryImage& binary)
{
    const int limit = std::max(binary.width, binary.height) / 4;
    std::vector<FinderCandidate> candidates;
    std::vector<std::pair<int, int>> runs;
    for (int y = 0; y < binary.height; ++y)
    {
        const std::uint8_t* row = binary.dark.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(binary.width);
        runs.clear();
        int start = 0;
        for (int x = 1; x <= binary.width; ++x)
        {
            if (x == binary.width || row[x] != row[start])
            {
                runs.emplace_back(start, x - start);
                start = x;
            }
        }
        for (std::size_t i = 0; i + 5 <= runs.size(); ++i)
        {
            if (row[runs[i].first] != 1)
            {
                continue;
            }
            const std::array<int, 5> lengths{runs[i].second, runs[i + 1].second, runs[i + 2].second, runs[i + 3].second, runs[i + 4].second};
            double module = 0.0;
            if (!CheckRatio(lengths, module))
            {
                continue;
            }

            const int columnX = runs[i + 2].first + runs[i + 2].second / 2;
            std::array<int, 5> vertical{};
            double centerY = 0.0;
            double moduleY = 0.0;
            if (!MeasureCross(binary, columnX, y, 0, 1, limit, vertical, centerY) || !CheckRatio(vertical, moduleY))
            {
                continue;
            }
            std::array<int, 5> horizontal{};
            double centerX = 0.0;
            double moduleX = 0.0;
            if (!MeasureCross(binary, columnX, static_cast<int>(centerY), 1, 0, limit, horizontal, centerX) ||
                !CheckRatio(horizontal, moduleX))
            {
                continue;
            }
            if (moduleX > 2.0 * moduleY || moduleY > 2.0 * moduleX)
            {
                continue;
            }
            candidates.push_back(FinderCandidate{centerX, centerY, (moduleX + moduleY) / 2.0});
        }
    }
    return candidates;
}

// Finds the four corner finders. Order: top-left, top-right, bottom-left, bottom-right.
bool FindFinders(const BinaryImage& binary, std::array<Point2, 4>& centers, double& module)
{
    std::vector<FinderCluster> clusters;
    for (const FinderCandidate& candidate : ScanFinderCandidates(binary))
    {
        bool merged = false;
        for (FinderCluster& cluster : clusters)
        {
            const double meanModule = cluster.sumModule / cluster.hits;
            const double dx = cluster.sumX / cluster.hits - candidate.x;
            const double dy = cluster.sumY / cluster.hits - candidate.y;
            if (std::hypot(dx, dy) < 2.0 * meanModule)
            {
                cluster.sumX += candidate.x;
                cluster.sumY += candidate.y;
                cluster.sumModule += candidate.module;
                ++cluster.hits;
                merged = true;
                break;
            }
        }
        if (!merged)
        {
            clusters.push_back(FinderCluster{candidate.x, candidate.y, candidate.module, 1});
        }
    }

    double largestModule = 0.0;
    for (const FinderCluster& cluster : clusters)
    {
        if (cluster.hits >= min_cluster_hits)
        {
            largestModule = std::max(largestModule, cluster.sumModule / cluster.hits);
        }
    }
    std::vector<FinderCandidate> finders;
    for (const FinderCluster& cluster : clusters)
    {
        const double meanModule = cluster.sumModule / cluster.hits;
        if (cluster.hits >= min_cluster_hits && meanModule >= min_module_share * largestModule)
        {
            finders.push_back(FinderCandidate{cluster.sumX / cluster.hits, cluster.sumY / cluster.hits, meanModule});
        }
    }
    if (finders.size() < 4)
    {
        return false;
    }

    // The real finders sit in the outermost corners of the frame.
    const std::array<std::pair<double, double>, 4> directions{std::pair{-1.0, -1.0}, std::pair{1.0, -1.0}, std::pair{-1.0, 1.0}, std::pair{1.0, 1.0}};
    std::array<std::size_t, 4> chosen{};
    for (std::size_t corner = 0; corner < 4; ++corner)
    {
        double best = 0.0;
        for (std::size_t i = 0; i < finders.size(); ++i)
        {
            const double score = directions[corner].first * finders[i].x + directions[corner].second * finders[i].y;
            if (i == 0 || score > best)
            {
                best = score;
                chosen[corner] = i;
            }
        }
    }
    module = 0.0;
    for (std::size_t corner = 0; corner < 4; ++corner)
    {
        for (std::size_t other = corner + 1; other < 4; ++other)
        {
            if (chosen[corner] == chosen[other])
            {
                return false;
            }
        }
        const FinderCandidate& finder = finders[chosen[corner]];
        centers[corner] = Point2{finder.x, finder.y};
        module += finder.module / 4.0;
    }
    return true;
}

Color SampleCell(const Image& image, const Homography& cellToImage, int cellX, int cellY)
{
    constexpr std::array<std::pair<double, double>, 5> offsets{
        std::pair{0.0, 0.0}, std::pair{-sample_spread, 0.0}, std::pair{sample_spread, 0.0}, std::pair{0.0, -sample_spread}, std::pair{0.0, sample_spread},
    };
    Color sum{};
    for (const auto& [ox, oy] : offsets)
    {
        const Point2 point = cellToImage.Map(Point2{cellX + 0.5 + ox, cellY + 0.5 + oy});
        const Color color = SampleBilinear(image, point.x, point.y);
        for (std::size_t c = 0; c < 3; ++c)
        {
            sum[c] += color[c] / static_cast<double>(offsets.size());
        }
    }
    return sum;
}

double Luma(const Color& color)
{
    return 0.299 * color[0] + 0.587 * color[1] + 0.114 * color[2];
}

HomographyCode FitGrid(const std::array<Point2, 4>& centers, int gridWidth, int gridHeight, Homography& cellToImage)
{
    const double finderCenter = finder_cells / 2.0;
    const std::array<Point2, 4> cellCenters{
        Point2{finderCenter, finderCenter},
        Point2{gridWidth - finderCenter, finderCenter},
        Point2{finderCenter, gridHeight - finderCenter},
        Point2{gridWidth - finderCenter, gridHeight - finderCenter},
    };
    return FitHomography(cellCenters, centers, cellToImage);
}

// How well the timing track of a candidate grid alternates: mean of (light ? +luma : -luma) over its cells.
double TimingScore(const Image& image, const std::array<Point2, 4>& centers, int gridWidth, int gridHeight, bool horizontal)
{
    Homography cellToImage{};
    if (FitGrid(centers, gridWidth, gridHeight, cellToImage) != HomographyCode::Ok)
    {
        return -1.0e9;
    }
    const int length = horizontal ? gridWidth : gridHeight;
    double score = 0.0;
    for (int i = corner_cells; i < length - corner_cells; ++i)
    {
        const double x = (horizontal ? i : finder_center_cell) + 0.5;
        const double y = (horizontal ? finder_center_cell : i) + 0.5;
        const Point2 point = cellToImage.Map(Point2{x, y});
        const double luma = Luma(SampleBilinear(image, point.x, point.y));
        score += i % 2 == 0 ? -luma : luma;
    }
    return score / std::max(1, length - 2 * corner_cells);
}

// Picks the even grid size near @p estimate whose timing track matches best.
int SearchGridSide(const Image& image, const std::array<Point2, 4>& centers, int estimate, int otherSide, bool horizontal)
{
    const int spread = std::max(4, estimate / 8);
    int best = estimate;
    double bestScore = -1.0e9;
    for (int candidate = (estimate - spread) & ~1; candidate <= estimate + spread; candidate += 2)
    {
        if (candidate < min_grid_cells)
        {
            continue;
        }
        const double score = horizontal ? TimingScore(image, centers, candidate, otherSide, true)
                                        : TimingScore(image, centers, otherSide, candidate, false);
        if (score > bestScore)
        {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

int EstimateGridSide(Point2 from, Point2 to, double cellPixels)
{
    const double centerDistance = std::hypot(to.x - from.x, to.y - from.y) / cellPixels;
    return static_cast<int>(std::lround(centerDistance + finder_cells)) & ~1;
}

// Sizes the grid for one assignment of image corners to frame corners; returns how well both timing tracks match.
double FitGridSize(const Image& image, const std::array<Point2, 4>& centers, double cellPixels, int& gridWidth, int& gridHeight)
{
    const int widthEstimate = EstimateGridSide(centers[0], centers[1], cellPixels);
    gridHeight = SearchGridSide(image, centers, EstimateGridSide(centers[0], centers[2], cellPixels), widthEstimate, false);
    gridWidth = SearchGridSide(image, centers, widthEstimate, gridHeight, true);
    gridHeight = SearchGridSide(image, centers, gridHeight, gridWidth, false);
    return TimingScore(image, centers, gridWidth, gridHeight, true) + TimingScore(image, centers, gridWidth, gridHeight, false);
}

int Classify(const Color& color, const std::vector<Color>& references)
{
    int best = 0;
    double bestDistance = 0.0;
    for (std::size_t i = 0; i < references.size(); ++i)
    {
        double distance = 0.0;
        for (std::size_t c = 0; c < 3; ++c)
        {
            const double delta = color[c] - references[i][c];
            distance += delta * delta;
        }
        if (i == 0 || distance < bestDistance)
        {
            best = static_cast<int>(i);
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace

FrameReader::FrameReader(int bitsPerCell)
    : bitsPerCell{bitsPerCell}
    , codec{rs_parity_bytes}
    , cachedLayout{}
{
}

ReadCode FrameReader::ReadFrame(const Image& image, FrameRead& out) const
{
    const BinaryImage binary = Binarize(image);
    std::array<Point2, 4> imageCorners{};
    double module = 0.0;
    if (!FindFinders(binary, imageCorners, module))
    {
        return ReadCode::FindersNotFound;
    }

    // The recording may be rotated: try each way the image corners can be the frame corners and keep the one
    // whose timing tracks match. Within each, the finder spacing sizes the grid roughly and the tracks pin it down.
    const double cellPixels = module / finder_module_cells;
    std::array<Point2, 4> centers{};
    int gridWidth = 0;
    int gridHeight = 0;
    double bestScore = 0.0;
    for (std::size_t rotation = 0; rotation < corner_rotations.size(); ++rotation)
    {
        std::array<Point2, 4> candidate{};
        for (std::size_t corner = 0; corner < 4; ++corner)
        {
            candidate[corner] = imageCorners[corner_rotations[rotation][corner]];
        }
        int candidateWidth = 0;
        int candidateHeight = 0;
        const double score = FitGridSize(image, candidate, cellPixels, candidateWidth, candidateHeight);
        if (rotation == 0 || score > bestScore)
        {
            bestScore = score;
            centers = candidate;
            gridWidth = candidateWidth;
            gridHeight = candidateHeight;
        }
    }
    if (cachedLayout.GridWidth() != gridWidth || cachedLayout.GridHeight() != gridHeight || cachedLayout.BitsPerCell() != bitsPerCell)
    {
        if (cachedLayout.Configure(gridWidth, gridHeight, bitsPerCell) != LayoutCode::Ok)
        {
            return ReadCode::UnsupportedGrid;
        }
    }
    const FrameLayout& layout = cachedLayout;

    Homography cellToImage{};
    if (FitGrid(centers, gridWidth, gridHeight, cellToImage) != HomographyCode::Ok)
    {
        return ReadCode::FindersNotFound;
    }

    // Learn the colours from the palette strips, using the middle cell of each run.
    std::vector<Color> references(static_cast<std::size_t>(layout.ColorCount()), Color{});
    std::vector<int> referenceCounts(references.size(), 0);
    for (const int y : {palette_row_offset, gridHeight - 1 - palette_row_offset})
    {
        for (int x = corner_cells; x < gridWidth - corner_cells; ++x)
        {
            if ((x - corner_cells) % palette_run_cells != palette_run_cells / 2)
            {
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(layout.ReservedColor(x, y, 0));
            const Color color = SampleCell(image, cellToImage, x, y);
            for (std::size_t c = 0; c < 3; ++c)
            {
                references[index][c] += color[c];
            }
            ++referenceCounts[index];
        }
    }
    for (std::size_t i = 0; i < references.size(); ++i)
    {
        if (referenceCounts[i] == 0)
        {
            return ReadCode::LowContrast;
        }
        for (std::size_t c = 0; c < 3; ++c)
        {
            references[i][c] /= referenceCounts[i];
        }
    }
    const double blackLuma = Luma(references[palette_black]);
    const double whiteLuma = Luma(references[palette_white]);
    if (whiteLuma - blackLuma < min_contrast)
    {
        return ReadCode::LowContrast;
    }

    // A capture that mixes two frames shows grey parity strips; one that tears shows different strips.
    std::array<double, 2> parityLevels{};
    const std::array<int, 2> parityRows{parity_row_offset, gridHeight - 1 - parity_row_offset};
    for (std::size_t strip = 0; strip < parityRows.size(); ++strip)
    {
        double sum = 0.0;
        int count = 0;
        for (int x = corner_cells; x < gridWidth - corner_cells; ++x)
        {
            sum += Luma(SampleCell(image, cellToImage, x, parityRows[strip]));
            ++count;
        }
        parityLevels[strip] = (sum / count - blackLuma) / (whiteLuma - blackLuma);
        if (parityLevels[strip] > blend_low && parityLevels[strip] < blend_high)
        {
            return ReadCode::BlendedFrame;
        }
    }
    if ((parityLevels[0] > 0.5) != (parityLevels[1] > 0.5))
    {
        return ReadCode::TornFrame;
    }

    const std::size_t codewords = static_cast<std::size_t>(layout.CodewordCount());
    const std::size_t cellsPerByte = 8 / static_cast<std::size_t>(bitsPerCell);
    std::vector<std::uint8_t> frameBytes(codewords * rs_codeword_bytes, 0);
    const std::span<const CellPoint> dataCells = layout.DataCells();
    const std::size_t usedCells = std::min(dataCells.size(), frameBytes.size() * cellsPerByte);
    for (std::size_t i = 0; i < usedCells; ++i)
    {
        const CellPoint cell = dataCells[i];
        const unsigned value = static_cast<unsigned>(Classify(SampleCell(image, cellToImage, cell.x, cell.y), references));
        const unsigned shift = static_cast<unsigned>(8 - bitsPerCell * static_cast<int>(i % cellsPerByte + 1));
        frameBytes[i / cellsPerByte] |= static_cast<std::uint8_t>(value << shift);
    }

    out.gridWidth = gridWidth;
    out.gridHeight = gridHeight;
    out.parity = parityLevels[0] > 0.5 ? 1U : 0U;
    out.payload.assign(layout.PayloadBytes(), 0);
    out.codewordOk.assign(codewords, false);
    out.correctedBytes = 0;
    out.failedCodewords = 0;
    out.packetsPerFrame = layout.PacketsPerFrame();
    out.packetBytes = layout.PacketBytes();
    std::vector<std::uint8_t> codeword(rs_codeword_bytes, 0);
    for (std::size_t c = 0; c < codewords; ++c)
    {
        for (std::size_t j = 0; j < rs_codeword_bytes; ++j)
        {
            codeword[j] = frameBytes[j * codewords + c];
        }
        int corrected = 0;
        if (codec.Decode(codeword, corrected) == RsCode::Ok)
        {
            out.codewordOk[c] = true;
            out.correctedBytes += corrected;
        }
        else
        {
            ++out.failedCodewords;
        }
        std::copy_n(codeword.begin(), rs_data_bytes, out.payload.begin() + static_cast<std::ptrdiff_t>(c * rs_data_bytes));
    }
    return ReadCode::Ok;
}

} // namespace vfdt
