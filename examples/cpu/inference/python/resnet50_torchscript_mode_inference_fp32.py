import torch
import torchvision.models as models

model = models.resnet50(weights='ResNet50_Weights.DEFAULT')
model.eval()
data = torch.rand(1, 3, 224, 224)

#################### code changes ####################
import intel_extension_for_pytorch as ipex
model = ipex.optimize(model)
######################################################

with torch.no_grad():
  d = torch.rand(1, 3, 224, 224)
  model = torch.jit.trace(model, d)
  model = torch.jit.freeze(model)

  model(data)