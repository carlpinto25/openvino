const { testModels, downloadTestModel } = require("./utils.js");

if (require.main === module) {
  main();
}

async function main() {
  await downloadTestModel(testModels.testModelFP32);
  await downloadTestModel(testModels.modelV3Small);
}
