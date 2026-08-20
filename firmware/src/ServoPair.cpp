#include "ServoPair.h"

void ServoPair::writeAngles(float xDeg, float yDeg) {
  x_.writeAngle(xDeg);
  y_.writeAngle(yDeg);
}
