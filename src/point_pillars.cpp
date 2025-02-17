#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <iostream>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <random>
#include <algorithm>
namespace py = pybind11;

struct IntPairHash {
  std::size_t operator()(const std::pair<uint32_t, uint32_t> &p) const {
    assert(sizeof(std::size_t)>=8);
    //Shift first integer over to make room for the second integer. The two are
    //then packed side by side.
    return (((uint64_t)p.first)<<32) | ((uint64_t)p.second);
  }
};

struct PillarPoint {
    int type;
    float x;
    float y;
    float xc;
    float yc;
    float xp;
    float yp;
    int step;
    float vx;
    float vy;
    float ax;
    float ay;
    float yaw;
    float omega;
};
// ponits type, x, y, step, vx, vy, ax, ay, yaw, omega [10D]
// enum AgentType {vehicle, pedestrian, road};

class OneHotEncoder{
    private:
        int num_categories = 0;
    public:
        OneHotEncoder(const int _num_categories){
            num_categories = _num_categories;
        }
        std::vector<float> encodeCategory(int category_index){
            std::string const msg = "ERROR: category index is"  + std::to_string(category_index) + "Number of cats" + std::to_string(num_categories);

            if (category_index >= num_categories)
                throw std::runtime_error("category_index must ve less than equal _num_categories) " + msg);
            
            std::vector<float> encoding(num_categories, 0.0);
            encoding.at(category_index) = float(1.0);
            return encoding;
        }
};

void validatePointPillar(PillarPoint p){
    if (p.type == 2){
        double _sum = abs(p.vx) + abs(p.vy) + abs(p.ax) + abs(p.ay) + abs(p.step+1) + abs(p.yaw) + abs(p.omega);
        if (_sum >= 0.0001)
            throw std::runtime_error("invalid data for a map point");
    }
    else if (p.type == 1)
    {
        double _sum = abs(p.yaw) + abs(p.omega);
        if (_sum >= 0.0001)
            throw std::runtime_error("invalid data for a pedestrian point");
    }
    
}

pybind11::tuple createPillars(pybind11::array_t<float> points,
                              int maxPointsPerPillar,
                              int maxPillars,
                              float xStep,
                              float yStep,
                              float xMin,
                              float xMax,
                              float yMin,
                              float yMax,
                              int max_steps,
                              int max_num_type,
                              bool printTime = false)
{   
    
    std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    int dimension = max_num_type + max_steps + 1 + 12 ; // fake step for map points
    if (points.ndim() != 3  || points.shape()[2] != 10)
    {
        throw std::runtime_error("numpy array with shape (batch_size, n,  10) expected (n being the number of points)");
    }

    int batch_size = points.shape()[0];
    pybind11::array_t<float> tensor;
    pybind11::array_t<int> indices;

    tensor.resize({batch_size, maxPillars, maxPointsPerPillar, dimension});
    indices.resize({batch_size, maxPillars, 3});

    for (int bi = 0; bi < batch_size; ++bi )
    {   

    std::unordered_map<std::pair<uint32_t, uint32_t>, std::vector<PillarPoint>, IntPairHash> map;
    for (int i = 0; i < points.shape()[1]; ++i)
    {   
        int type = (int)points.at(bi, i,0);
        if ( type < 0 || type >= max_num_type )
            throw std::runtime_error("Error; type is not valid");

        auto x = points.at(bi, i, 1);
        auto y = points.at(bi, i, 2);

        int step = (int)points.at(bi, i, 3);
        if ((type == 0 || type == 1) && (step < 0 || step >= max_steps))
            throw std::runtime_error("Error: invalid step number");
        
        float vx = points.at(bi, i, 4);
        float vy = points.at(bi, i, 5);
        float ax = points.at(bi, i, 6);
        float ay = points.at(bi, i, 7);

        float yaw = points.at(bi, i, 8);
        yaw = atan2(sin(yaw), cos(yaw));

        float omega = points.at(bi, i, 9);

        
        if (x < xMin || x >= xMax || y < yMin || y >= yMax)
        {
            continue;
        }

        auto xIndex = static_cast<uint32_t>(std::floor((x - xMin) / xStep));
        auto yIndex = static_cast<uint32_t>(std::floor((y - yMin) / yStep));

        // point in local coordinate frame of each pillar
        auto xp = x - ( xIndex * xStep + xMin ); 
        auto yp = y - ( yIndex * yStep + yMin );

        PillarPoint p = {
            type,
            x,
            y,
            0, //xc
            0, //yc
            xp,
            yp,
            step,
            vx, // vx
            vy, // vy
            ax, // ax
            ay, // ay
            yaw,
            omega, // omega
        };
        validatePointPillar(p);

        map[{xIndex, yIndex}].emplace_back(p);
    }

    auto rng = std::default_random_engine {};

    int pillarId = 0;
    for (auto& pair: map)
    {
        if (pillarId >= maxPillars)
        {
            break;
        }

        float xMean = 0;
        float yMean = 0;
        for (const auto& p: pair.second)
        {
            xMean += p.x;
            yMean += p.y;
        }
        xMean /= pair.second.size();
        yMean /= pair.second.size();

        for (auto& p: pair.second)
        {
            p.xc = p.x - xMean;
            p.yc = p.y - yMean;
        }

        auto xIndex = static_cast<int>(std::floor((xMean - xMin) / xStep));
        auto yIndex = static_cast<int>(std::floor((yMean - yMin) / yStep));
        int max_x_index = static_cast<int>(std::floor((xMax-0.0001 - xMin) / xStep));
        int max_y_index = static_cast<int>(std::floor((yMax-0.0001 - yMin) / yStep));

        indices.mutable_at(bi, pillarId, 1) = max_y_index-yIndex;  // Image coordinate modification (swapped)
        indices.mutable_at(bi, pillarId, 2) = xIndex;  // Image coordinate modification (swapped)

        int pointId = 0;

        // shuffle to have all kind of points sampled
        std::shuffle(std::begin(pair.second), std::end(pair.second), rng);

        for (const auto& p: pair.second)
        {
            if (pointId >= maxPointsPerPillar)
            {
                break;
            }

            // type encoding
            auto type_encoder = OneHotEncoder(max_num_type);
            auto encoded_type = type_encoder.encodeCategory(p.type);
            for (int i = 0; i < max_num_type; i++)
            {
                tensor.mutable_at(bi, pillarId, pointId, i) = encoded_type.at(i); 
            }

            int j = max_num_type;
            tensor.mutable_at(bi, pillarId, pointId, j+0) = p.x - (0.5 * max_x_index * xStep + xMin);
            tensor.mutable_at(bi, pillarId, pointId, j+1) = p.y - (0.5 * max_y_index * yStep + yMin);
            tensor.mutable_at(bi, pillarId, pointId, j+2) = p.xc;
            tensor.mutable_at(bi, pillarId, pointId, j+3) = p.yc;
            tensor.mutable_at(bi, pillarId, pointId, j+4) = p.xp;
            tensor.mutable_at(bi, pillarId, pointId, j+5) = p.yp;

            // time encoding
            auto time_encoder = OneHotEncoder(max_steps+1);
            std::vector<float> encoded_time;
            auto cf = 1.0;
            if (p.type ==2){ // map elements
                encoded_time = time_encoder.encodeCategory(0); // +1 is for the map points where ste=-1
                
            }
            else{
                encoded_time = time_encoder.encodeCategory(p.step+1); // +1 is for the map points where ste=-1
            }
            j = max_num_type + 6; // 9
            for (int i = 0; i < max_steps+1; i++)
            {
                tensor.mutable_at(bi, pillarId, pointId, i + j) = encoded_time.at(i); 
            }
            j = max_num_type + 6 + max_steps + 1; // 20
            
            tensor.mutable_at(bi, pillarId, pointId, j+0) = p.vx; // 20
            tensor.mutable_at(bi, pillarId, pointId, j+1) = p.vy; // 21
            tensor.mutable_at(bi, pillarId, pointId, j+2) = p.ax; // 22
            tensor.mutable_at(bi, pillarId, pointId, j+3) = p.ay; //23
            tensor.mutable_at(bi, pillarId, pointId, j+4) = p.yaw; //24
            tensor.mutable_at(bi, pillarId, pointId, j+5) = p.omega; //25

            pointId++;
        }

        pillarId++;
    }
    }

    pybind11::tuple result = pybind11::make_tuple(tensor, indices);

    std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>( t2 - t1 ).count();
    if (printTime)
        std::cout << "createPillars took: " << static_cast<float>(duration) / 1e6 << " seconds" << std::endl;

    return result;
}

// pybind11::tuple createPillars(pybind11::array_t<float> points,
//                               int maxPointsPerPillar,
//                               int maxPillars,
//                               float xStep,
//                               float yStep,
//                               float xMin,
//                               float xMax,
//                               float yMin,
//                               float yMax,
//                               int max_steps,
//                               int max_num_type,
//                               bool printTime = false)
// {   
//     if (points.ndim() != 3 || points.shape()[2] != 10)
//     {
//         throw std::runtime_error("numpy array with shape (batch_size, n,  10) expected (n being the number of points)");
//     }

//     int batch_size = points.shape()[0];
//     pybind11::array_t<float> tensor;
//     pybind11::array_t<int> indices;
//     tensor.resize({batch_size, maxPillars, maxPointsPerPillar, dimension});
//     indices.resize({batch_size, maxPillars, 3});

//     for (int i = 0; i < batch_size; i++)
//     {
//         auto result = createPillars_internal(points, maxPointsPerPillar, maxPillars, 
//                                                 xStep, yStep, xMin, xMax, yMin, yMax,
//                                                 max_steps, max_num_type, printTime);
//         pybind11::array_t<float> tensor_a;
//         pybind11::array_t<float> tensor_b;
//         tensor_a, tensor_b = *result;
//         tensor.mutable_at(i, pybind11::ellipsis()) = tensor_a;
//         indices.mutable_at(i, pybind11::ellipsis()) = tensor_b;
        
//     }

//     pybind11::tuple result = pybind11::make_tuple(tensor, indices);
    
//     return result;
// }

struct BoundingBox3D
{
    float x;
    float y;
    float z;
    float length;
    float width;
    float height;
    float yaw;
    float classId;
};

struct Point2D {
    float x;
    float y;
};

typedef std::vector<Point2D> Polyline2D;

// Returns x-value of point of intersection of two lines
float xIntersect(float x1, float y1, float x2, float y2,
                     float x3, float y3, float x4, float y4)
{
    float num = (x1*y2 - y1*x2) * (x3-x4) - (x1-x2) * (x3*y4 - y3*x4);
    float den = (x1-x2) * (y3-y4) - (y1-y2) * (x3-x4);
    return num/den;
}

// Returns y-value of point of intersection of two lines
float yIntersect(float x1, float y1, float x2, float y2,
                     float x3, float y3, float x4, float y4)
{
    float num = (x1*y2 - y1*x2) * (y3-y4) - (y1-y2) * (x3*y4 - y3*x4);
    float den = (x1-x2) * (y3-y4) - (y1-y2) * (x3-x4);
    return num/den;
}

// Returns area of polygon using the shoelace method
float polygonArea(const Polyline2D &polygon)
{
    float area = 0.0;

    size_t j = polygon.size()-1;
    for (size_t i = 0; i < polygon.size(); i++)
    {
        area += (polygon[j].x + polygon[i].x) * (polygon[j].y - polygon[i].y);
        j = i;  // j is previous vertex to i
    }

    return std::abs(area / 2.0); // Return absolute value
}

float rotatedX(float x, float y, float angle)
{
    return x * std::cos(angle) - y * std::sin(angle);
}

float rotatedY(float x, float y, float angle)
{
    return x * std::sin(angle) + y * std::cos(angle);
}

// Construct bounding box in 2D, coordinates are returned in clockwise order
Polyline2D boundingBox3DToTopDown(const BoundingBox3D &box1)
{
    Polyline2D box;
    box.push_back({rotatedX(-0.5 * box1.width, 0.5 * box1.length,
                            box1.yaw) + box1.x,
                   rotatedY(-0.5 * box1.width, 0.5 * box1.length,
                            box1.yaw) + box1.y});

    box.push_back({rotatedX(0.5 * box1.width, 0.5 * box1.length,
                            box1.yaw) + box1.x,
                   rotatedY(0.5 * box1.width, 0.5 * box1.length,
                            box1.yaw) + box1.y});

    box.push_back({rotatedX(0.5 * box1.width, -0.5 * box1.length,
                            box1.yaw) + box1.x,
                   rotatedY(0.5 * box1.width, -0.5 * box1.length,
                            box1.yaw) + box1.y});

    box.push_back({rotatedX(-0.5 * box1.width, -0.5 * box1.length,
                            box1.yaw) + box1.x,
                   rotatedY(-0.5 * box1.width, -0.5 * box1.length,
                            box1.yaw) + box1.y});

    return box;
}

// This functions clips all the edges w.r.t one Clip edge of clipping area
// Returns a clipped polygon...
Polyline2D clip(const Polyline2D &poly_points,
                float x1,
                float y1,
                float x2,
                float y2)
{
    Polyline2D new_points;

    for (size_t i = 0; i < poly_points.size(); i++)
    {
        // (ix,iy),(kx,ky) are the co-ordinate values of the points
        // i and k form a line in polygon
        size_t k = (i+1) % poly_points.size();
        float ix = poly_points[i].x, iy = poly_points[i].y;
        float kx = poly_points[k].x, ky = poly_points[k].y;

        // Calculating position of first point w.r.t. clipper line
        float i_pos = (x2-x1) * (iy-y1) - (y2-y1) * (ix-x1);

        // Calculating position of second point w.r.t. clipper line
        float k_pos = (x2-x1) * (ky-y1) - (y2-y1) * (kx-x1);

        // Case 1 : When both points are inside
        if (i_pos < 0  && k_pos < 0)
        {
            //Only second point is added
            new_points.push_back({kx,ky});
        }

            // Case 2: When only first point is outside
        else if (i_pos >= 0  && k_pos < 0)
        {
            // Point of intersection with edge
            // and the second point is added
            new_points.push_back({xIntersect(x1, y1, x2, y2, ix, iy, kx, ky),
                                  yIntersect(x1, y1, x2, y2, ix, iy, kx, ky)});
            new_points.push_back({kx,ky});

        }

            // Case 3: When only second point is outside
        else if (i_pos < 0  && k_pos >= 0)
        {
            //Only point of intersection with edge is added
            new_points.push_back({xIntersect(x1, y1, x2, y2, ix, iy, kx, ky),
                                  yIntersect(x1, y1, x2, y2, ix, iy, kx, ky)});

        }
            // Case 4: When both points are outside
        else
        {
            //No points are added
        }
    }

    return new_points;
}

// Implements Sutherland–Hodgman algorithm
// Returns a polygon with the intersection between two polygons.
Polyline2D sutherlandHodgmanClip(const Polyline2D &poly_points_vector,
                                 const Polyline2D &clipper_points)
{
    Polyline2D clipped_poly_points_vector = poly_points_vector;
    for (size_t i=0; i<clipper_points.size(); i++)
    {
        size_t k = (i+1) % clipper_points.size(); //i and k are two consecutive indexes

        // We pass the current array of vertices, and the end points of the selected clipper line
        clipped_poly_points_vector = clip(clipped_poly_points_vector, clipper_points[i].x, clipper_points[i].y,
                                          clipper_points[k].x, clipper_points[k].y);
    }
    return clipped_poly_points_vector;
}

// Calculates the IOU between two bounding boxes.
float iou(const BoundingBox3D& box1,
          const BoundingBox3D& box2)
{
    const auto& box_as_vector = boundingBox3DToTopDown(box1);
    const auto& box_as_vector_2 = boundingBox3DToTopDown(box2);
    const auto& clipped_vector = sutherlandHodgmanClip(box_as_vector, box_as_vector_2);

    float area_poly1 = polygonArea(box_as_vector);
    float area_poly2 = polygonArea(box_as_vector_2);
    float area_overlap = polygonArea(clipped_vector);

    return area_overlap / (area_poly1 + area_poly2 - area_overlap);
}

int clip(int n, int lower, int upper) {
  return std::max(lower, std::min(n, upper));
}

std::tuple<pybind11::array_t<float>, int, int> createPillarsTarget(const pybind11::array_t<float>& objectPositions,
                                             const pybind11::array_t<float>& objectDimensions,
                                             const pybind11::array_t<float>& objectYaws,
                                             const pybind11::array_t<int>& objectClassIds,
                                             const pybind11::array_t<float>& anchorDimensions,
                                             const pybind11::array_t<float>& anchorZHeights,
                                             const pybind11::array_t<float>& anchorYaws,
                                             float positiveThreshold,
                                             float negativeThreshold,
                                             int nbClasses,
                                             int downscalingFactor,
                                             float xStep,
                                             float yStep,
                                             float xMin,
                                             float xMax,
                                             float yMin,
                                             float yMax,
                                             float zMin,
                                             float zMax,
                                             bool printTime = false)
{
    
    std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();

    const auto xSize = static_cast<int>(std::floor((xMax - xMin) / (xStep * downscalingFactor)));
//     py::print("xSize", xSize);
    const auto ySize = static_cast<int>(std::floor((yMax - yMin) / (yStep * downscalingFactor)));
//     py::print("ySize", ySize);

    const int nbAnchors = anchorDimensions.shape()[0]; //4 Number of anchors
//     py::print("nbAnchors", nbAnchors);
//     Anchor length

    if (nbAnchors <= 0)
    {
        throw std::runtime_error("Anchor length is zero");
    }

    const int nbObjects = objectDimensions.shape()[0]; //6 Number of labels inside a label.txt file
//     BB dimensions from the label file
    if (nbObjects <= 0)
    {
        throw std::runtime_error("Object length is zero");
    }
//     py::print("nbObjects", nbObjects);

    // parse numpy arrays
//     Preparing the anchor bounding box
    std::vector<BoundingBox3D> anchorBoxes = {};
    std::vector<float> anchorDiagonals;
    for (int i = 0; i < nbAnchors; ++i)
    {
        BoundingBox3D anchorBox = {};
        anchorBox.x = 0;
        anchorBox.y = 0;
        anchorBox.length = anchorDimensions.at(i, 0);
        anchorBox.width = anchorDimensions.at(i, 1);
        anchorBox.height = anchorDimensions.at(i, 2);
        anchorBox.z = anchorZHeights.at(i);
        anchorBox.yaw = anchorYaws.at(i);
        anchorBoxes.emplace_back(anchorBox); // Appends a new anchorBox to the AnchorBoxes container
        // Note that anchor box doesn't have a classId as of now.
        anchorDiagonals.emplace_back(std::sqrt(std::pow(anchorBox.width, 2) + std::pow(anchorBox.length, 2)));
    }

//     Preparing the label bounding box
    std::vector<BoundingBox3D> labelBoxes = {};
    for (int i = 0; i < nbObjects; ++i)
    {
        float x = objectPositions.at(i, 0);
        float y = objectPositions.at(i, 1);
        if (x < xMin | x > xMax | y < yMin | y > yMax)
        {
            continue;
        }
        BoundingBox3D labelBox = {};
        labelBox.x = x;
        labelBox.y = y;
        labelBox.z = objectPositions.at(i, 2);
        labelBox.length = objectDimensions.at(i, 0);
        labelBox.width = objectDimensions.at(i, 1);
        labelBox.height = objectDimensions.at(i, 2);
        labelBox.yaw = objectYaws.at(i);
        labelBox.classId = objectClassIds.at(i);
        labelBoxes.emplace_back(labelBox);
    }

    pybind11::array_t<float> tensor;
    tensor.resize({nbObjects, xSize, ySize, nbAnchors, 10}); //Tensor of size (6,252,252,4,10) for first file
    
    pybind11::buffer_info tensor_buffer = tensor.request();
    float *ptr1 = (float *) tensor_buffer.ptr;
    // Zero filling the tensor. Every element is presently zero
    for (size_t idx = 0; idx < nbObjects * xSize * ySize * nbAnchors * 10; idx++)
    {
        ptr1[idx] = 0;
    }

    int posCnt = 0;
    int negCnt = 0;
    int objectCount = 0;
    if (printTime)
    {
//         std::cout << "Received " << labelBoxes.size() << " objects" << std::endl;
//         py::print("Received "+str(labelBoxes.size())+" objects");
    }
    for (const auto& labelBox: labelBoxes) //For every label box which is a 3d bounding box
    {
        // zone-in on potential spatial area of interest
        // Length of (width,length) axis diagonal.
        float objectDiameter = std::sqrt(std::pow(labelBox.width, 2) + std::pow(labelBox.length, 2));
        // Offset = Number of grid boxes that can fit on the object diameter
        const auto x_offset = static_cast<int>(std::ceil(objectDiameter / (xStep * downscalingFactor)));
        const auto y_offset = static_cast<int>(std::ceil(objectDiameter / (yStep * downscalingFactor)));
        // Xc = Number of grid boxes that can fit between Xmin (Ymin) and Label's x (y) coordinate
        const auto xC = static_cast<int>(std::floor((labelBox.x - xMin) / (xStep * downscalingFactor)));
        const auto yC = static_cast<int>(std::floor((labelBox.y - yMin) / (yStep * downscalingFactor)));
        // X(Y)Start = Start from Xc (Yc) - Number of boxes in object's diameter.
        // For example the object is located at 5 unites and is 2 unites long. Then X(Y)start will begin
        // the search from 3
        const auto xStart = clip(xC - x_offset, 0, xSize);
        const auto yStart = clip(yC - y_offset, 0, ySize);
        // Similarly end the search at 8 units. Because the object cannot extend beyond that.
        const auto xEnd = clip(xC + x_offset, 0, xSize);
        const auto yEnd = clip(yC + y_offset, 0, ySize);

        float maxIou = 0;
        BoundingBox3D bestAnchor = {};
        int bestAnchorId = 0;
        for (int xId = xStart; xId < xEnd; xId++) // Iterate through every box within search diameter
            // In our example case, from 3 till 8
        {
            const float x = xId * xStep * downscalingFactor + xMin;
            // Getting the real world x coordinate
            for (int yId = yStart; yId < yEnd; yId++) // Iterate through every box within search diamter in y axis
            {
                const float y = yId * yStep * downscalingFactor + yMin;
                // Get the real world y coordinates
                int anchorCount = 0;
                for (auto& anchorBox: anchorBoxes) // For every anchor box (4 in our case)
                    // Note that we are checking every anchor box for every label in the file
                {
                    anchorBox.x = x; // Assign the real world x and y coordinate to the anchor box
                    anchorBox.y = y; // Note that anchor boxes originally didn't have Xs and Ys.
                    // This is because we need ot check them along the X-Y grid.
                    // However, they did have a z value attached to them. 

                    const float iouOverlap = iou(anchorBox, labelBox); // Get IOU between two 3D boxes.

                    if (maxIou < iouOverlap)
                    {
                        maxIou = iouOverlap;
                        bestAnchor = anchorBox;
                        bestAnchorId = anchorCount;
                    }

                    if (iouOverlap > positiveThreshold) // Accept the Anchor. Add the anchor details to the tensor.
                    {
                        // Tensor at CurrentObject Id, xth grid cell, yth grid cell, currentAnchor, 0
                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 0) = 1; 

                        auto diag = anchorDiagonals[anchorCount];
                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 1) = (labelBox.x - anchorBox.x) / diag; // delta x,y,z
                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 2) = (labelBox.y - anchorBox.y) / diag;
                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 3) = (labelBox.z - anchorBox.z) / anchorBox.height;

                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 4) = std::log(labelBox.length / anchorBox.length); // delta l,w,h
                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 5) = std::log(labelBox.width / anchorBox.width);
                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 6) = std::log(labelBox.height / anchorBox.height);

                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 7) = std::sin(labelBox.yaw - anchorBox.yaw); //delta yaw
                        if (labelBox.yaw > 0) // Is yaw > 0
                        {
                            tensor.mutable_at(objectCount, xId, yId, anchorCount, 8) = 1; 
                        }
                        else
                        {
                            tensor.mutable_at(objectCount, xId, yId, anchorCount, 8) = 0;
                        }

                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 9) = labelBox.classId;

                    }
                    else if (iouOverlap < negativeThreshold)
                    {
                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 0) = 0;
                    }
                    else
                    {
                        tensor.mutable_at(objectCount, xId, yId, anchorCount, 0) = -1;
                    }

                    anchorCount++;
                }
            }
        }

        if (maxIou < positiveThreshold) // Comparing maxIOU for that object obtained after checking with every anchor box
            // If none of the anchors passed the threshold, then we place the best anchor details for that object. 
        {
            negCnt++;
            if (printTime)
            {
//                 std::cout << "\nThere was no sufficiently overlapping anchor anywhere for object " << objectCount << std::endl;
//                 py::print("There was no sufficiently overlapping anchor anywhere for object " +str(objectCount));
//                 std::cout << "Best IOU was " << maxIou << ". Adding the best location regardless of threshold." << std::endl;
//                 py::print("Best IOU was "+str(maxIou)+" Adding the best location regardless of threshold");
            }

            const auto xId = static_cast<int>(std::floor((labelBox.x - xMin) / (xStep * downscalingFactor)));
            const auto yId = static_cast<int>(std::floor((labelBox.y - yMin) / (yStep * downscalingFactor)));
            const float diag = std::sqrt(std::pow(bestAnchor.width, 2) + std::pow(bestAnchor.length, 2));

            tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 0) = 1;

            tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 1) = (labelBox.x - bestAnchor.x) / diag;
            tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 2) = (labelBox.y - bestAnchor.y) / diag;
            tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 3) = (labelBox.z - bestAnchor.z) / bestAnchor.height;

            tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 4) = std::log(labelBox.length / bestAnchor.length);
            tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 5) = std::log(labelBox.width / bestAnchor.width);
            tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 6) = std::log(labelBox.height / bestAnchor.height);

            tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 7) = std::sin(labelBox.yaw - bestAnchor.yaw);
            if (labelBox.yaw > 0)
            {
                tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 8) = 1;
            }
            else
            {
                tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 8) = 0;
            }
//             Class id is the classification label (0,1,2,3)
            tensor.mutable_at(objectCount, xId, yId, bestAnchorId, 9) = labelBox.classId;
        }
        else
        {
            posCnt++;
            if (printTime)
            {
                std::cout << "\nAt least 1 anchor was positively matched for object " << objectCount << std::endl;
                std::cout << "Best IOU was " << maxIou << "." << std::endl;
            }
        }

        objectCount++;
    }

    std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>( t2 - t1 ).count();
    if (printTime)
        std::cout << "createPillarsTarget took: " << static_cast<float>(duration) / 1e6 << " seconds" << std::endl;

    return std::make_tuple(tensor, posCnt, negCnt);
}


PYBIND11_MODULE(point_pillars, m)
{
    m.def("createPillars", &createPillars, "Runs function to create point pillars input tensors");
    m.def("createPillarsTarget", &createPillarsTarget, "Runs function to create point pillars output ground truth");
}
