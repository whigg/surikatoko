Requires:
C++17 due to usage of std::filesystem

Requires: gflags, glog, (Guideline) GSL, Eigen, OpenCV
Optional: Pangolin, gtest

Tested on:
Windows, VS2017 v141
Ubuntu, GCC 8.3

Run C++ demos:
```
demo-circle-grid.exe --logtostderr -v 4
demo-dino.exe --logtostderr -v 4 --testdata ../../../../testdata
demo-davison-mono-slam --flagfile=flagfile-demo-davison-mono-slam.txt
```
Davison's MonoSlam implementation.

Implementation of MonoSlam by Andrew Davison https://www.doc.ic.ac.uk/~ajd/Scene/index.html
The algorithm uses Kalman filter to estimate camera's location and map features (salient points).
The algorithm is described in the book "Structure from Motion using the Extended Kalman Filter" Civera 2011.

The uncertainty ellipsoid of a salient points are rendered with different colors to indicate:
Green - new salient points, added to the map;
Red - the salient point is matched to the one from previous frame.
Yellow - unobserved salient point.
Almost spherical ellipsoids are drawn with two extra strokes on the flanks.

3D world view. The view renders an uncertainty ellipsoid, associated with each salient point. The ellipsoids are not linked to any camera (as there may be multiple cameras observing the scene) and have no uncertainty, derived from uncertainty of camera position and orientation.
2D camera view. As 3D position of every salient point is projected onto the camera, in the same way the uncertainty is propagated from 3D world coordinates into 2D picture coordinates. The resultant 2D ellipse brings uncertainty, associated with position and orientation of the camera. Thus 3D world view and 2D camera view show uncertainties which are visually different.
The rectangle in the image where a salient point is expected to be in the next frame (and where it is searched) is drawn with gray color.
Camera view shows only salient points which fit the current view, the 3D view renders all salient points.
World view renders salient points' patches by back-projecting 2D blobs in the picture, back into the 3D world. Hence the patch is linked to the frame of a camera. The salient point itself is rendered as a dot (small solid rectangle) in the coordinates, estimated by Kalman Filter. Thus some salient point may have the 3D center and patch coordinates diverge and visually do not overlap. This indicates some inconsistencies in the filtering process.
2D ellipse:
Solid: projection of an ellipsoid which is in forefront of the camera;
Dashed: projection of ellipsoid which cuts camera plane;
Dotted: approximation of projection of ellipsoid, created by projecting a couple of 3D points which lie on the ellipsoid boundary.

Hot keys:
f=next frame
i=dumps camera information to the console
u=(virtual mode only) sets the position of the camera  and salient points to the ground truth
s='skip' current frame - performs Kalman filter's 'prediction' step without 'update' step (as if camera is covered with a blanket)

Testing:
Set env(SRK_TEST_DATA) to point to SRC/testdata folder.